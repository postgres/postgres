/*
 * re_*comp and friends - compile REs
 * This file #includes several others (see the bottom).
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
 * $PostgreSQL: pgsql/src/backend/regex/regcomp.c,v 1.44 2005/10/15 02:49:24 momjian Exp $
 *
 */

#include "regex/regguts.h"

/*
 * forward declarations, up here so forward datatypes etc. are defined early
 */
/* === regcomp.c === */
static void moresubs(struct vars *, int);
static int	freev(struct vars *, int);
static void makesearch(struct vars *, struct nfa *);
static struct subre *parse(struct vars *, int, int, struct state *, struct state *);
static struct subre *parsebranch(struct vars *, int, int, struct state *, struct state *, int);
static void parseqatom(struct vars *, int, int, struct state *, struct state *, struct subre *);
static void nonword(struct vars *, int, struct state *, struct state *);
static void word(struct vars *, int, struct state *, struct state *);
static int	scannum(struct vars *);
static void repeat(struct vars *, struct state *, struct state *, int, int);
static void bracket(struct vars *, struct state *, struct state *);
static void cbracket(struct vars *, struct state *, struct state *);
static void brackpart(struct vars *, struct state *, struct state *);
static chr *scanplain(struct vars *);
static void leaders(struct vars *, struct cvec *);
static void onechr(struct vars *, chr, struct state *, struct state *);
static void dovec(struct vars *, struct cvec *, struct state *, struct state *);
static celt nextleader(struct vars *, chr, chr);
static void wordchrs(struct vars *);
static struct subre *subre(struct vars *, int, int, struct state *, struct state *);
static void freesubre(struct vars *, struct subre *);
static void freesrnode(struct vars *, struct subre *);
static void optst(struct vars *, struct subre *);
static int	numst(struct subre *, int);
static void markst(struct subre *);
static void cleanst(struct vars *);
static long nfatree(struct vars *, struct subre *, FILE *);
static long nfanode(struct vars *, struct subre *, FILE *);
static int	newlacon(struct vars *, struct state *, struct state *, int);
static void freelacons(struct subre *, int);
static void rfree(regex_t *);

#ifdef REG_DEBUG
static void dump(regex_t *, FILE *);
static void dumpst(struct subre *, FILE *, int);
static void stdump(struct subre *, FILE *, int);
static char *stid(struct subre *, char *, size_t);
#endif
/* === regc_lex.c === */
static void lexstart(struct vars *);
static void prefixes(struct vars *);
static void lexnest(struct vars *, chr *, chr *);
static void lexword(struct vars *);
static int	next(struct vars *);
static int	lexescape(struct vars *);
static chr	lexdigits(struct vars *, int, int, int);
static int	brenext(struct vars *, chr);
static void skip(struct vars *);
static chr	newline(void);
static chr	chrnamed(struct vars *, chr *, chr *, chr);

/* === regc_color.c === */
static void initcm(struct vars *, struct colormap *);
static void freecm(struct colormap *);
static void cmtreefree(struct colormap *, union tree *, int);
static color setcolor(struct colormap *, chr, pcolor);
static color maxcolor(struct colormap *);
static color newcolor(struct colormap *);
static void freecolor(struct colormap *, pcolor);
static color pseudocolor(struct colormap *);
static color subcolor(struct colormap *, chr c);
static color newsub(struct colormap *, pcolor);
static void subrange(struct vars *, chr, chr, struct state *, struct state *);
static void subblock(struct vars *, chr, struct state *, struct state *);
static void okcolors(struct nfa *, struct colormap *);
static void colorchain(struct colormap *, struct arc *);
static void uncolorchain(struct colormap *, struct arc *);
static int	singleton(struct colormap *, chr c);
static void rainbow(struct nfa *, struct colormap *, int, pcolor, struct state *, struct state *);
static void colorcomplement(struct nfa *, struct colormap *, int, struct state *, struct state *, struct state *);

#ifdef REG_DEBUG
static void dumpcolors(struct colormap *, FILE *);
static void fillcheck(struct colormap *, union tree *, int, FILE *);
static void dumpchr(chr, FILE *);
#endif
/* === regc_nfa.c === */
static struct nfa *newnfa(struct vars *, struct colormap *, struct nfa *);
static void freenfa(struct nfa *);
static struct state *newstate(struct nfa *);
static struct state *newfstate(struct nfa *, int flag);
static void dropstate(struct nfa *, struct state *);
static void freestate(struct nfa *, struct state *);
static void destroystate(struct nfa *, struct state *);
static void newarc(struct nfa *, int, pcolor, struct state *, struct state *);
static struct arc *allocarc(struct nfa *, struct state *);
static void freearc(struct nfa *, struct arc *);
static struct arc *findarc(struct state *, int, pcolor);
static void cparc(struct nfa *, struct arc *, struct state *, struct state *);
static void moveins(struct nfa *, struct state *, struct state *);
static void copyins(struct nfa *, struct state *, struct state *);
static void moveouts(struct nfa *, struct state *, struct state *);
static void copyouts(struct nfa *, struct state *, struct state *);
static void cloneouts(struct nfa *, struct state *, struct state *, struct state *, int);
static void delsub(struct nfa *, struct state *, struct state *);
static void deltraverse(struct nfa *, struct state *, struct state *);
static void dupnfa(struct nfa *, struct state *, struct state *, struct state *, struct state *);
static void duptraverse(struct nfa *, struct state *, struct state *);
static void cleartraverse(struct nfa *, struct state *);
static void specialcolors(struct nfa *);
static long optimize(struct nfa *, FILE *);
static void pullback(struct nfa *, FILE *);
static int	pull(struct nfa *, struct arc *);
static void pushfwd(struct nfa *, FILE *);
static int	push(struct nfa *, struct arc *);

#define INCOMPATIBLE	1		/* destroys arc */
#define SATISFIED	2			/* constraint satisfied */
#define COMPATIBLE	3			/* compatible but not satisfied yet */
static int	combine(struct arc *, struct arc *);
static void fixempties(struct nfa *, FILE *);
static int	unempty(struct nfa *, struct arc *);
static void cleanup(struct nfa *);
static void markreachable(struct nfa *, struct state *, struct state *, struct state *);
static void markcanreach(struct nfa *, struct state *, struct state *, struct state *);
static long analyze(struct nfa *);
static void compact(struct nfa *, struct cnfa *);
static void carcsort(struct carc *, struct carc *);
static void freecnfa(struct cnfa *);
static void dumpnfa(struct nfa *, FILE *);

#ifdef REG_DEBUG
static void dumpstate(struct state *, FILE *);
static void dumparcs(struct state *, FILE *);
static int	dumprarcs(struct arc *, struct state *, FILE *, int);
static void dumparc(struct arc *, struct state *, FILE *);
static void dumpcnfa(struct cnfa *, FILE *);
static void dumpcstate(int, struct carc *, struct cnfa *, FILE *);
#endif
/* === regc_cvec.c === */
static struct cvec *newcvec(int, int, int);
static struct cvec *clearcvec(struct cvec *);
static void addchr(struct cvec *, chr);
static void addrange(struct cvec *, chr, chr);
static void addmcce(struct cvec *, chr *, chr *);
static int	haschr(struct cvec *, chr);
static struct cvec *getcvec(struct vars *, int, int, int);
static void freecvec(struct cvec *);

/* === regc_locale.c === */
static int	pg_wc_isdigit(pg_wchar c);
static int	pg_wc_isalpha(pg_wchar c);
static int	pg_wc_isalnum(pg_wchar c);
static int	pg_wc_isupper(pg_wchar c);
static int	pg_wc_islower(pg_wchar c);
static int	pg_wc_isgraph(pg_wchar c);
static int	pg_wc_isprint(pg_wchar c);
static int	pg_wc_ispunct(pg_wchar c);
static int	pg_wc_isspace(pg_wchar c);
static pg_wchar pg_wc_toupper(pg_wchar c);
static pg_wchar pg_wc_tolower(pg_wchar c);
static int	nmcces(struct vars *);
static int	nleaders(struct vars *);
static struct cvec *allmcces(struct vars *, struct cvec *);
static celt element(struct vars *, chr *, chr *);
static struct cvec *range(struct vars *, celt, celt, int);
static int	before(celt, celt);
static struct cvec *eclass(struct vars *, celt, int);
static struct cvec *cclass(struct vars *, chr *, chr *, int);
static struct cvec *allcases(struct vars *, chr);
static int	cmp(const chr *, const chr *, size_t);
static int	casecmp(const chr *, const chr *, size_t);


/* internal variables, bundled for easy passing around */
struct vars
{
	regex_t    *re;
	chr		   *now;			/* scan pointer into string */
	chr		   *stop;			/* end of string */
	chr		   *savenow;		/* saved now and stop for "subroutine call" */
	chr		   *savestop;
	int			err;			/* error code (0 if none) */
	int			cflags;			/* copy of compile flags */
	int			lasttype;		/* type of previous token */
	int			nexttype;		/* type of next token */
	chr			nextvalue;		/* value (if any) of next token */
	int			lexcon;			/* lexical context type (see lex.c) */
	int			nsubexp;		/* subexpression count */
	struct subre **subs;		/* subRE pointer vector */
	size_t		nsubs;			/* length of vector */
	struct subre *sub10[10];	/* initial vector, enough for most */
	struct nfa *nfa;			/* the NFA */
	struct colormap *cm;		/* character color map */
	color		nlcolor;		/* color of newline */
	struct state *wordchrs;		/* state in nfa holding word-char outarcs */
	struct subre *tree;			/* subexpression tree */
	struct subre *treechain;	/* all tree nodes allocated */
	struct subre *treefree;		/* any free tree nodes */
	int			ntree;			/* number of tree nodes */
	struct cvec *cv;			/* interface cvec */
	struct cvec *cv2;			/* utility cvec */
	struct cvec *mcces;			/* collating-element information */
#define  ISCELEADER(v,c) ((v)->mcces != NULL && haschr((v)->mcces, (c)))
	struct state *mccepbegin;	/* in nfa, start of MCCE prototypes */
	struct state *mccepend;		/* in nfa, end of MCCE prototypes */
	struct subre *lacons;		/* lookahead-constraint vector */
	int			nlacons;		/* size of lacons */
};

/* parsing macros; most know that `v' is the struct vars pointer */
#define NEXT()	(next(v))		/* advance by one token */
#define SEE(t)	(v->nexttype == (t))	/* is next token this? */
#define EAT(t)	(SEE(t) && next(v))		/* if next is this, swallow it */
#define VISERR(vv)	((vv)->err != 0)	/* have we seen an error yet? */
#define ISERR() VISERR(v)
#define VERR(vv,e)	((vv)->nexttype = EOS, ((vv)->err) ? (vv)->err :\
							((vv)->err = (e)))
#define ERR(e)	VERR(v, e)		/* record an error */
#define NOERR() {if (ISERR()) return;}	/* if error seen, return */
#define NOERRN()	{if (ISERR()) return NULL;} /* NOERR with retval */
#define NOERRZ()	{if (ISERR()) return 0;}	/* NOERR with retval */
#define INSIST(c, e)	((c) ? 0 : ERR(e))		/* if condition false, error */
#define NOTE(b) (v->re->re_info |= (b)) /* note visible condition */
#define EMPTYARC(x, y)	newarc(v->nfa, EMPTY, 0, x, y)

/* token type codes, some also used as NFA arc types */
#define EMPTY	'n'				/* no token present */
#define EOS 'e'					/* end of string */
#define PLAIN	'p'				/* ordinary character */
#define DIGIT	'd'				/* digit (in bound) */
#define BACKREF 'b'				/* back reference */
#define COLLEL	'I'				/* start of [. */
#define ECLASS	'E'				/* start of [= */
#define CCLASS	'C'				/* start of [: */
#define END 'X'					/* end of [. [= [: */
#define RANGE	'R'				/* - within [] which might be range delim. */
#define LACON	'L'				/* lookahead constraint subRE */
#define AHEAD	'a'				/* color-lookahead arc */
#define BEHIND	'r'				/* color-lookbehind arc */
#define WBDRY	'w'				/* word boundary constraint */
#define NWBDRY	'W'				/* non-word-boundary constraint */
#define SBEGIN	'A'				/* beginning of string (even if not BOL) */
#define SEND	'Z'				/* end of string (even if not EOL) */
#define PREFER	'P'				/* length preference */

/* is an arc colored, and hence on a color chain? */
#define COLORED(a)	((a)->type == PLAIN || (a)->type == AHEAD || \
							(a)->type == BEHIND)



/* static function list */
static struct fns functions = {
	rfree,						/* regfree insides */
};



/*
 * pg_regcomp - compile regular expression
 */
int
pg_regcomp(regex_t *re,
		   const chr *string,
		   size_t len,
		   int flags)
{
	struct vars var;
	struct vars *v = &var;
	struct guts *g;
	int			i;
	size_t		j;

#ifdef REG_DEBUG
	FILE	   *debug = (flags & REG_PROGRESS) ? stdout : (FILE *) NULL;
#else
	FILE	   *debug = (FILE *) NULL;
#endif

#define  CNOERR()	 { if (ISERR()) return freev(v, v->err); }

	/* sanity checks */

	if (re == NULL || string == NULL)
		return REG_INVARG;
	if ((flags & REG_QUOTE) &&
		(flags & (REG_ADVANCED | REG_EXPANDED | REG_NEWLINE)))
		return REG_INVARG;
	if (!(flags & REG_EXTENDED) && (flags & REG_ADVF))
		return REG_INVARG;

	/* initial setup (after which freev() is callable) */
	v->re = re;
	v->now = (chr *) string;
	v->stop = v->now + len;
	v->savenow = v->savestop = NULL;
	v->err = 0;
	v->cflags = flags;
	v->nsubexp = 0;
	v->subs = v->sub10;
	v->nsubs = 10;
	for (j = 0; j < v->nsubs; j++)
		v->subs[j] = NULL;
	v->nfa = NULL;
	v->cm = NULL;
	v->nlcolor = COLORLESS;
	v->wordchrs = NULL;
	v->tree = NULL;
	v->treechain = NULL;
	v->treefree = NULL;
	v->cv = NULL;
	v->cv2 = NULL;
	v->mcces = NULL;
	v->lacons = NULL;
	v->nlacons = 0;
	re->re_magic = REMAGIC;
	re->re_info = 0;			/* bits get set during parse */
	re->re_csize = sizeof(chr);
	re->re_guts = NULL;
	re->re_fns = VS(&functions);

	/* more complex setup, malloced things */
	re->re_guts = VS(MALLOC(sizeof(struct guts)));
	if (re->re_guts == NULL)
		return freev(v, REG_ESPACE);
	g = (struct guts *) re->re_guts;
	g->tree = NULL;
	initcm(v, &g->cmap);
	v->cm = &g->cmap;
	g->lacons = NULL;
	g->nlacons = 0;
	ZAPCNFA(g->search);
	v->nfa = newnfa(v, v->cm, (struct nfa *) NULL);
	CNOERR();
	v->cv = newcvec(100, 20, 10);
	if (v->cv == NULL)
		return freev(v, REG_ESPACE);
	i = nmcces(v);
	if (i > 0)
	{
		v->mcces = newcvec(nleaders(v), 0, i);
		CNOERR();
		v->mcces = allmcces(v, v->mcces);
		leaders(v, v->mcces);
		addmcce(v->mcces, (chr *) NULL, (chr *) NULL);	/* dummy */
	}
	CNOERR();

	/* parsing */
	lexstart(v);				/* also handles prefixes */
	if ((v->cflags & REG_NLSTOP) || (v->cflags & REG_NLANCH))
	{
		/* assign newline a unique color */
		v->nlcolor = subcolor(v->cm, newline());
		okcolors(v->nfa, v->cm);
	}
	CNOERR();
	v->tree = parse(v, EOS, PLAIN, v->nfa->init, v->nfa->final);
	assert(SEE(EOS));			/* even if error; ISERR() => SEE(EOS) */
	CNOERR();
	assert(v->tree != NULL);

	/* finish setup of nfa and its subre tree */
	specialcolors(v->nfa);
	CNOERR();
#ifdef REG_DEBUG
	if (debug != NULL)
	{
		fprintf(debug, "\n\n\n========= RAW ==========\n");
		dumpnfa(v->nfa, debug);
		dumpst(v->tree, debug, 1);
	}
#endif
	optst(v, v->tree);
	v->ntree = numst(v->tree, 1);
	markst(v->tree);
	cleanst(v);
#ifdef REG_DEBUG
	if (debug != NULL)
	{
		fprintf(debug, "\n\n\n========= TREE FIXED ==========\n");
		dumpst(v->tree, debug, 1);
	}
#endif

	/* build compacted NFAs for tree and lacons */
	re->re_info |= nfatree(v, v->tree, debug);
	CNOERR();
	assert(v->nlacons == 0 || v->lacons != NULL);
	for (i = 1; i < v->nlacons; i++)
	{
#ifdef REG_DEBUG
		if (debug != NULL)
			fprintf(debug, "\n\n\n========= LA%d ==========\n", i);
#endif
		nfanode(v, &v->lacons[i], debug);
	}
	CNOERR();
	if (v->tree->flags & SHORTER)
		NOTE(REG_USHORTEST);

	/* build compacted NFAs for tree, lacons, fast search */
#ifdef REG_DEBUG
	if (debug != NULL)
		fprintf(debug, "\n\n\n========= SEARCH ==========\n");
#endif
	/* can sacrifice main NFA now, so use it as work area */
	(DISCARD) optimize(v->nfa, debug);
	CNOERR();
	makesearch(v, v->nfa);
	CNOERR();
	compact(v->nfa, &g->search);
	CNOERR();

	/* looks okay, package it up */
	re->re_nsub = v->nsubexp;
	v->re = NULL;				/* freev no longer frees re */
	g->magic = GUTSMAGIC;
	g->cflags = v->cflags;
	g->info = re->re_info;
	g->nsub = re->re_nsub;
	g->tree = v->tree;
	v->tree = NULL;
	g->ntree = v->ntree;
	g->compare = (v->cflags & REG_ICASE) ? casecmp : cmp;
	g->lacons = v->lacons;
	v->lacons = NULL;
	g->nlacons = v->nlacons;

#ifdef REG_DEBUG
	if (flags & REG_DUMP)
		dump(re, stdout);
#endif

	assert(v->err == 0);
	return freev(v, 0);
}

/*
 * moresubs - enlarge subRE vector
 */
static void
moresubs(struct vars * v,
		 int wanted)			/* want enough room for this one */
{
	struct subre **p;
	size_t		n;

	assert(wanted > 0 && (size_t) wanted >= v->nsubs);
	n = (size_t) wanted *3 / 2 + 1;

	if (v->subs == v->sub10)
	{
		p = (struct subre **) MALLOC(n * sizeof(struct subre *));
		if (p != NULL)
			memcpy(VS(p), VS(v->subs),
				   v->nsubs * sizeof(struct subre *));
	}
	else
		p = (struct subre **) REALLOC(v->subs, n * sizeof(struct subre *));
	if (p == NULL)
	{
		ERR(REG_ESPACE);
		return;
	}
	v->subs = p;
	for (p = &v->subs[v->nsubs]; v->nsubs < n; p++, v->nsubs++)
		*p = NULL;
	assert(v->nsubs == n);
	assert((size_t) wanted < v->nsubs);
}

/*
 * freev - free vars struct's substructures where necessary
 *
 * Optionally does error-number setting, and always returns error code
 * (if any), to make error-handling code terser.
 */
static int
freev(struct vars * v,
	  int err)
{
	if (v->re != NULL)
		rfree(v->re);
	if (v->subs != v->sub10)
		FREE(v->subs);
	if (v->nfa != NULL)
		freenfa(v->nfa);
	if (v->tree != NULL)
		freesubre(v, v->tree);
	if (v->treechain != NULL)
		cleanst(v);
	if (v->cv != NULL)
		freecvec(v->cv);
	if (v->cv2 != NULL)
		freecvec(v->cv2);
	if (v->mcces != NULL)
		freecvec(v->mcces);
	if (v->lacons != NULL)
		freelacons(v->lacons, v->nlacons);
	ERR(err);					/* nop if err==0 */

	return v->err;
}

/*
 * makesearch - turn an NFA into a search NFA (implicit prepend of .*?)
 * NFA must have been optimize()d already.
 */
static void
makesearch(struct vars * v,
		   struct nfa * nfa)
{
	struct arc *a;
	struct arc *b;
	struct state *pre = nfa->pre;
	struct state *s;
	struct state *s2;
	struct state *slist;

	/* no loops are needed if it's anchored */
	for (a = pre->outs; a != NULL; a = a->outchain)
	{
		assert(a->type == PLAIN);
		if (a->co != nfa->bos[0] && a->co != nfa->bos[1])
			break;
	}
	if (a != NULL)
	{
		/* add implicit .* in front */
		rainbow(nfa, v->cm, PLAIN, COLORLESS, pre, pre);

		/* and ^* and \A* too -- not always necessary, but harmless */
		newarc(nfa, PLAIN, nfa->bos[0], pre, pre);
		newarc(nfa, PLAIN, nfa->bos[1], pre, pre);
	}

	/*
	 * Now here's the subtle part.  Because many REs have no lookback
	 * constraints, often knowing when you were in the pre state tells you
	 * little; it's the next state(s) that are informative.  But some of them
	 * may have other inarcs, i.e. it may be possible to make actual progress
	 * and then return to one of them.	We must de-optimize such cases,
	 * splitting each such state into progress and no-progress states.
	 */

	/* first, make a list of the states */
	slist = NULL;
	for (a = pre->outs; a != NULL; a = a->outchain)
	{
		s = a->to;
		for (b = s->ins; b != NULL; b = b->inchain)
			if (b->from != pre)
				break;
		if (b != NULL)
		{						/* must be split */
			if (s->tmp == NULL)
			{					/* if not already in the list */
				/* (fixes bugs 505048, 230589, */
				/* 840258, 504785) */
				s->tmp = slist;
				slist = s;
			}
		}
	}

	/* do the splits */
	for (s = slist; s != NULL; s = s2)
	{
		s2 = newstate(nfa);
		copyouts(nfa, s, s2);
		for (a = s->ins; a != NULL; a = b)
		{
			b = a->inchain;
			if (a->from != pre)
			{
				cparc(nfa, a, a->from, s2);
				freearc(nfa, a);
			}
		}
		s2 = s->tmp;
		s->tmp = NULL;			/* clean up while we're at it */
	}
}

/*
 * parse - parse an RE
 *
 * This is actually just the top level, which parses a bunch of branches
 * tied together with '|'.	They appear in the tree as the left children
 * of a chain of '|' subres.
 */
static struct subre *
parse(struct vars * v,
	  int stopper,				/* EOS or ')' */
	  int type,					/* LACON (lookahead subRE) or PLAIN */
	  struct state * init,		/* initial state */
	  struct state * final)		/* final state */
{
	struct state *left;			/* scaffolding for branch */
	struct state *right;
	struct subre *branches;		/* top level */
	struct subre *branch;		/* current branch */
	struct subre *t;			/* temporary */
	int			firstbranch;	/* is this the first branch? */

	assert(stopper == ')' || stopper == EOS);

	branches = subre(v, '|', LONGER, init, final);
	NOERRN();
	branch = branches;
	firstbranch = 1;
	do
	{							/* a branch */
		if (!firstbranch)
		{
			/* need a place to hang it */
			branch->right = subre(v, '|', LONGER, init, final);
			NOERRN();
			branch = branch->right;
		}
		firstbranch = 0;
		left = newstate(v->nfa);
		right = newstate(v->nfa);
		NOERRN();
		EMPTYARC(init, left);
		EMPTYARC(right, final);
		NOERRN();
		branch->left = parsebranch(v, stopper, type, left, right, 0);
		NOERRN();
		branch->flags |= UP(branch->flags | branch->left->flags);
		if ((branch->flags & ~branches->flags) != 0)	/* new flags */
			for (t = branches; t != branch; t = t->right)
				t->flags |= branch->flags;
	} while (EAT('|'));
	assert(SEE(stopper) || SEE(EOS));

	if (!SEE(stopper))
	{
		assert(stopper == ')' && SEE(EOS));
		ERR(REG_EPAREN);
	}

	/* optimize out simple cases */
	if (branch == branches)
	{							/* only one branch */
		assert(branch->right == NULL);
		t = branch->left;
		branch->left = NULL;
		freesubre(v, branches);
		branches = t;
	}
	else if (!MESSY(branches->flags))
	{							/* no interesting innards */
		freesubre(v, branches->left);
		branches->left = NULL;
		freesubre(v, branches->right);
		branches->right = NULL;
		branches->op = '=';
	}

	return branches;
}

/*
 * parsebranch - parse one branch of an RE
 *
 * This mostly manages concatenation, working closely with parseqatom().
 * Concatenated things are bundled up as much as possible, with separate
 * ',' nodes introduced only when necessary due to substructure.
 */
static struct subre *
parsebranch(struct vars * v,
			int stopper,		/* EOS or ')' */
			int type,			/* LACON (lookahead subRE) or PLAIN */
			struct state * left,	/* leftmost state */
			struct state * right,		/* rightmost state */
			int partial)		/* is this only part of a branch? */
{
	struct state *lp;			/* left end of current construct */
	int			seencontent;	/* is there anything in this branch yet? */
	struct subre *t;

	lp = left;
	seencontent = 0;
	t = subre(v, '=', 0, left, right);	/* op '=' is tentative */
	NOERRN();
	while (!SEE('|') && !SEE(stopper) && !SEE(EOS))
	{
		if (seencontent)
		{						/* implicit concat operator */
			lp = newstate(v->nfa);
			NOERRN();
			moveins(v->nfa, right, lp);
		}
		seencontent = 1;

		/* NB, recursion in parseqatom() may swallow rest of branch */
		parseqatom(v, stopper, type, lp, right, t);
	}

	if (!seencontent)
	{							/* empty branch */
		if (!partial)
			NOTE(REG_UUNSPEC);
		assert(lp == left);
		EMPTYARC(left, right);
	}

	return t;
}

/*
 * parseqatom - parse one quantified atom or constraint of an RE
 *
 * The bookkeeping near the end cooperates very closely with parsebranch();
 * in particular, it contains a recursion that can involve parsing the rest
 * of the branch, making this function's name somewhat inaccurate.
 */
static void
parseqatom(struct vars * v,
		   int stopper,			/* EOS or ')' */
		   int type,			/* LACON (lookahead subRE) or PLAIN */
		   struct state * lp,	/* left state to hang it on */
		   struct state * rp,	/* right state to hang it on */
		   struct subre * top)	/* subtree top */
{
	struct state *s;			/* temporaries for new states */
	struct state *s2;

#define  ARCV(t, val)	 newarc(v->nfa, t, val, lp, rp)
	int			m,
				n;
	struct subre *atom;			/* atom's subtree */
	struct subre *t;
	int			cap;			/* capturing parens? */
	int			pos;			/* positive lookahead? */
	int			subno;			/* capturing-parens or backref number */
	int			atomtype;
	int			qprefer;		/* quantifier short/long preference */
	int			f;
	struct subre **atomp;		/* where the pointer to atom is */

	/* initial bookkeeping */
	atom = NULL;
	assert(lp->nouts == 0);		/* must string new code */
	assert(rp->nins == 0);		/* between lp and rp */
	subno = 0;					/* just to shut lint up */

	/* an atom or constraint... */
	atomtype = v->nexttype;
	switch (atomtype)
	{
			/* first, constraints, which end by returning */
		case '^':
			ARCV('^', 1);
			if (v->cflags & REG_NLANCH)
				ARCV(BEHIND, v->nlcolor);
			NEXT();
			return;
			break;
		case '$':
			ARCV('$', 1);
			if (v->cflags & REG_NLANCH)
				ARCV(AHEAD, v->nlcolor);
			NEXT();
			return;
			break;
		case SBEGIN:
			ARCV('^', 1);		/* BOL */
			ARCV('^', 0);		/* or BOS */
			NEXT();
			return;
			break;
		case SEND:
			ARCV('$', 1);		/* EOL */
			ARCV('$', 0);		/* or EOS */
			NEXT();
			return;
			break;
		case '<':
			wordchrs(v);		/* does NEXT() */
			s = newstate(v->nfa);
			NOERR();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			return;
			break;
		case '>':
			wordchrs(v);		/* does NEXT() */
			s = newstate(v->nfa);
			NOERR();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			return;
			break;
		case WBDRY:
			wordchrs(v);		/* does NEXT() */
			s = newstate(v->nfa);
			NOERR();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERR();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			return;
			break;
		case NWBDRY:
			wordchrs(v);		/* does NEXT() */
			s = newstate(v->nfa);
			NOERR();
			word(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERR();
			nonword(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			return;
			break;
		case LACON:				/* lookahead constraint */
			pos = v->nextvalue;
			NEXT();
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERR();
			t = parse(v, ')', LACON, s, s2);
			freesubre(v, t);	/* internal structure irrelevant */
			assert(SEE(')') || ISERR());
			NEXT();
			n = newlacon(v, s, s2, pos);
			NOERR();
			ARCV(LACON, n);
			return;
			break;
			/* then errors, to get them out of the way */
		case '*':
		case '+':
		case '?':
		case '{':
			ERR(REG_BADRPT);
			return;
			break;
		default:
			ERR(REG_ASSERT);
			return;
			break;
			/* then plain characters, and minor variants on that theme */
		case ')':				/* unbalanced paren */
			if ((v->cflags & REG_ADVANCED) != REG_EXTENDED)
			{
				ERR(REG_EPAREN);
				return;
			}
			/* legal in EREs due to specification botch */
			NOTE(REG_UPBOTCH);
			/* fallthrough into case PLAIN */
		case PLAIN:
			onechr(v, v->nextvalue, lp, rp);
			okcolors(v->nfa, v->cm);
			NOERR();
			NEXT();
			break;
		case '[':
			if (v->nextvalue == 1)
				bracket(v, lp, rp);
			else
				cbracket(v, lp, rp);
			assert(SEE(']') || ISERR());
			NEXT();
			break;
		case '.':
			rainbow(v->nfa, v->cm, PLAIN,
					(v->cflags & REG_NLSTOP) ? v->nlcolor : COLORLESS,
					lp, rp);
			NEXT();
			break;
			/* and finally the ugly stuff */
		case '(':				/* value flags as capturing or non */
			cap = (type == LACON) ? 0 : v->nextvalue;
			if (cap)
			{
				v->nsubexp++;
				subno = v->nsubexp;
				if ((size_t) subno >= v->nsubs)
					moresubs(v, subno);
				assert((size_t) subno < v->nsubs);
			}
			else
				atomtype = PLAIN;		/* something that's not '(' */
			NEXT();
			/* need new endpoints because tree will contain pointers */
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERR();
			EMPTYARC(lp, s);
			EMPTYARC(s2, rp);
			NOERR();
			atom = parse(v, ')', PLAIN, s, s2);
			assert(SEE(')') || ISERR());
			NEXT();
			NOERR();
			if (cap)
			{
				v->subs[subno] = atom;
				t = subre(v, '(', atom->flags | CAP, lp, rp);
				NOERR();
				t->subno = subno;
				t->left = atom;
				atom = t;
			}
			/* postpone everything else pending possible {0} */
			break;
		case BACKREF:			/* the Feature From The Black Lagoon */
			INSIST(type != LACON, REG_ESUBREG);
			INSIST(v->nextvalue < v->nsubs, REG_ESUBREG);
			INSIST(v->subs[v->nextvalue] != NULL, REG_ESUBREG);
			NOERR();
			assert(v->nextvalue > 0);
			atom = subre(v, 'b', BACKR, lp, rp);
			subno = v->nextvalue;
			atom->subno = subno;
			EMPTYARC(lp, rp);	/* temporarily, so there's something */
			NEXT();
			break;
	}

	/* ...and an atom may be followed by a quantifier */
	switch (v->nexttype)
	{
		case '*':
			m = 0;
			n = INFINITY;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '+':
			m = 1;
			n = INFINITY;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '?':
			m = 0;
			n = 1;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '{':
			NEXT();
			m = scannum(v);
			if (EAT(','))
			{
				if (SEE(DIGIT))
					n = scannum(v);
				else
					n = INFINITY;
				if (m > n)
				{
					ERR(REG_BADBR);
					return;
				}
				/* {m,n} exercises preference, even if it's {m,m} */
				qprefer = (v->nextvalue) ? LONGER : SHORTER;
			}
			else
			{
				n = m;
				/* {m} passes operand's preference through */
				qprefer = 0;
			}
			if (!SEE('}'))
			{					/* catches errors too */
				ERR(REG_BADBR);
				return;
			}
			NEXT();
			break;
		default:				/* no quantifier */
			m = n = 1;
			qprefer = 0;
			break;
	}

	/* annoying special case:  {0} or {0,0} cancels everything */
	if (m == 0 && n == 0)
	{
		if (atom != NULL)
			freesubre(v, atom);
		if (atomtype == '(')
			v->subs[subno] = NULL;
		delsub(v->nfa, lp, rp);
		EMPTYARC(lp, rp);
		return;
	}

	/* if not a messy case, avoid hard part */
	assert(!MESSY(top->flags));
	f = top->flags | qprefer | ((atom != NULL) ? atom->flags : 0);
	if (atomtype != '(' && atomtype != BACKREF && !MESSY(UP(f)))
	{
		if (!(m == 1 && n == 1))
			repeat(v, lp, rp, m, n);
		if (atom != NULL)
			freesubre(v, atom);
		top->flags = f;
		return;
	}

	/*
	 * hard part:  something messy That is, capturing parens, back reference,
	 * short/long clash, or an atom with substructure containing one of those.
	 */

	/* now we'll need a subre for the contents even if they're boring */
	if (atom == NULL)
	{
		atom = subre(v, '=', 0, lp, rp);
		NOERR();
	}

	/*
	 * prepare a general-purpose state skeleton
	 *
	 * ---> [s] ---prefix---> [begin] ---atom---> [end] ----rest---> [rp] / /
	 * [lp] ----> [s2] ----bypass---------------------
	 *
	 * where bypass is an empty, and prefix is some repetitions of atom
	 */
	s = newstate(v->nfa);		/* first, new endpoints for the atom */
	s2 = newstate(v->nfa);
	NOERR();
	moveouts(v->nfa, lp, s);
	moveins(v->nfa, rp, s2);
	NOERR();
	atom->begin = s;
	atom->end = s2;
	s = newstate(v->nfa);		/* and spots for prefix and bypass */
	s2 = newstate(v->nfa);
	NOERR();
	EMPTYARC(lp, s);
	EMPTYARC(lp, s2);
	NOERR();

	/* break remaining subRE into x{...} and what follows */
	t = subre(v, '.', COMBINE(qprefer, atom->flags), lp, rp);
	t->left = atom;
	atomp = &t->left;
	/* here we should recurse... but we must postpone that to the end */

	/* split top into prefix and remaining */
	assert(top->op == '=' && top->left == NULL && top->right == NULL);
	top->left = subre(v, '=', top->flags, top->begin, lp);
	top->op = '.';
	top->right = t;

	/* if it's a backref, now is the time to replicate the subNFA */
	if (atomtype == BACKREF)
	{
		assert(atom->begin->nouts == 1);		/* just the EMPTY */
		delsub(v->nfa, atom->begin, atom->end);
		assert(v->subs[subno] != NULL);
		/* and here's why the recursion got postponed:  it must */
		/* wait until the skeleton is filled in, because it may */
		/* hit a backref that wants to copy the filled-in skeleton */
		dupnfa(v->nfa, v->subs[subno]->begin, v->subs[subno]->end,
			   atom->begin, atom->end);
		NOERR();
	}

	/* it's quantifier time; first, turn x{0,...} into x{1,...}|empty */
	if (m == 0)
	{
		EMPTYARC(s2, atom->end);	/* the bypass */
		assert(PREF(qprefer) != 0);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '|', f, lp, atom->end);
		NOERR();
		t->left = atom;
		t->right = subre(v, '|', PREF(f), s2, atom->end);
		NOERR();
		t->right->left = subre(v, '=', 0, s2, atom->end);
		NOERR();
		*atomp = t;
		atomp = &t->left;
		m = 1;
	}

	/* deal with the rest of the quantifier */
	if (atomtype == BACKREF)
	{
		/* special case:  backrefs have internal quantifiers */
		EMPTYARC(s, atom->begin);		/* empty prefix */
		/* just stuff everything into atom */
		repeat(v, atom->begin, atom->end, m, n);
		atom->min = (short) m;
		atom->max = (short) n;
		atom->flags |= COMBINE(qprefer, atom->flags);
	}
	else if (m == 1 && n == 1)
	{
		/* no/vacuous quantifier:  done */
		EMPTYARC(s, atom->begin);		/* empty prefix */
	}
	else
	{
		/* turn x{m,n} into x{m-1,n-1}x, with capturing */
		/* parens in only second x */
		dupnfa(v->nfa, atom->begin, atom->end, s, atom->begin);
		assert(m >= 1 && m != INFINITY && n >= 1);
		repeat(v, s, atom->begin, m - 1, (n == INFINITY) ? n : n - 1);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '.', f, s, atom->end);		/* prefix and atom */
		NOERR();
		t->left = subre(v, '=', PREF(f), s, atom->begin);
		NOERR();
		t->right = atom;
		*atomp = t;
	}

	/* and finally, look after that postponed recursion */
	t = top->right;
	if (!(SEE('|') || SEE(stopper) || SEE(EOS)))
		t->right = parsebranch(v, stopper, type, atom->end, rp, 1);
	else
	{
		EMPTYARC(atom->end, rp);
		t->right = subre(v, '=', 0, atom->end, rp);
	}
	assert(SEE('|') || SEE(stopper) || SEE(EOS));
	t->flags |= COMBINE(t->flags, t->right->flags);
	top->flags |= COMBINE(top->flags, t->flags);
}

/*
 * nonword - generate arcs for non-word-character ahead or behind
 */
static void
nonword(struct vars * v,
		int dir,				/* AHEAD or BEHIND */
		struct state * lp,
		struct state * rp)
{
	int			anchor = (dir == AHEAD) ? '$' : '^';

	assert(dir == AHEAD || dir == BEHIND);
	newarc(v->nfa, anchor, 1, lp, rp);
	newarc(v->nfa, anchor, 0, lp, rp);
	colorcomplement(v->nfa, v->cm, dir, v->wordchrs, lp, rp);
	/* (no need for special attention to \n) */
}

/*
 * word - generate arcs for word character ahead or behind
 */
static void
word(struct vars * v,
	 int dir,					/* AHEAD or BEHIND */
	 struct state * lp,
	 struct state * rp)
{
	assert(dir == AHEAD || dir == BEHIND);
	cloneouts(v->nfa, v->wordchrs, lp, rp, dir);
	/* (no need for special attention to \n) */
}

/*
 * scannum - scan a number
 */
static int						/* value, <= DUPMAX */
scannum(struct vars * v)
{
	int			n = 0;

	while (SEE(DIGIT) && n < DUPMAX)
	{
		n = n * 10 + v->nextvalue;
		NEXT();
	}
	if (SEE(DIGIT) || n > DUPMAX)
	{
		ERR(REG_BADBR);
		return 0;
	}
	return n;
}

/*
 * repeat - replicate subNFA for quantifiers
 *
 * The duplication sequences used here are chosen carefully so that any
 * pointers starting out pointing into the subexpression end up pointing into
 * the last occurrence.  (Note that it may not be strung between the same
 * left and right end states, however!)  This used to be important for the
 * subRE tree, although the important bits are now handled by the in-line
 * code in parse(), and when this is called, it doesn't matter any more.
 */
static void
repeat(struct vars * v,
	   struct state * lp,
	   struct state * rp,
	   int m,
	   int n)
{
#define  SOME	 2
#define  INF 3
#define  PAIR(x, y)  ((x)*4 + (y))
#define  REDUCE(x)	 ( ((x) == INFINITY) ? INF : (((x) > 1) ? SOME : (x)) )
	const int	rm = REDUCE(m);
	const int	rn = REDUCE(n);
	struct state *s;
	struct state *s2;

	switch (PAIR(rm, rn))
	{
		case PAIR(0, 0):		/* empty string */
			delsub(v->nfa, lp, rp);
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, 1):		/* do as x| */
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, SOME):		/* do as x{1,n}| */
			repeat(v, lp, rp, 1, n);
			NOERR();
			EMPTYARC(lp, rp);
			break;
		case PAIR(0, INF):		/* loop x around */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			moveins(v->nfa, rp, s);
			EMPTYARC(lp, s);
			EMPTYARC(s, rp);
			break;
		case PAIR(1, 1):		/* no action required */
			break;
		case PAIR(1, SOME):		/* do as x{0,n-1}x = (x{1,n-1}|)x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, 1, n - 1);
			NOERR();
			EMPTYARC(lp, s);
			break;
		case PAIR(1, INF):		/* add loopback arc */
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			moveins(v->nfa, rp, s2);
			EMPTYARC(lp, s);
			EMPTYARC(s2, rp);
			EMPTYARC(s2, s);
			break;
		case PAIR(SOME, SOME):	/* do as x{m-1,n-1}x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, m - 1, n - 1);
			break;
		case PAIR(SOME, INF):	/* do as x{m-1,}x */
			s = newstate(v->nfa);
			NOERR();
			moveouts(v->nfa, lp, s);
			dupnfa(v->nfa, s, rp, lp, s);
			NOERR();
			repeat(v, lp, s, m - 1, n);
			break;
		default:
			ERR(REG_ASSERT);
			break;
	}
}

/*
 * bracket - handle non-complemented bracket expression
 * Also called from cbracket for complemented bracket expressions.
 */
static void
bracket(struct vars * v,
		struct state * lp,
		struct state * rp)
{
	assert(SEE('['));
	NEXT();
	while (!SEE(']') && !SEE(EOS))
		brackpart(v, lp, rp);
	assert(SEE(']') || ISERR());
	okcolors(v->nfa, v->cm);
}

/*
 * cbracket - handle complemented bracket expression
 * We do it by calling bracket() with dummy endpoints, and then complementing
 * the result.	The alternative would be to invoke rainbow(), and then delete
 * arcs as the b.e. is seen... but that gets messy.
 */
static void
cbracket(struct vars * v,
		 struct state * lp,
		 struct state * rp)
{
	struct state *left = newstate(v->nfa);
	struct state *right = newstate(v->nfa);
	struct state *s;
	struct arc *a;				/* arc from lp */
	struct arc *ba;				/* arc from left, from bracket() */
	struct arc *pa;				/* MCCE-prototype arc */
	color		co;
	chr		   *p;
	int			i;

	NOERR();
	bracket(v, left, right);
	if (v->cflags & REG_NLSTOP)
		newarc(v->nfa, PLAIN, v->nlcolor, left, right);
	NOERR();

	assert(lp->nouts == 0);		/* all outarcs will be ours */

	/* easy part of complementing */
	colorcomplement(v->nfa, v->cm, PLAIN, left, lp, rp);
	NOERR();
	if (v->mcces == NULL)
	{							/* no MCCEs -- we're done */
		dropstate(v->nfa, left);
		assert(right->nins == 0);
		freestate(v->nfa, right);
		return;
	}

	/* but complementing gets messy in the presence of MCCEs... */
	NOTE(REG_ULOCALE);
	for (p = v->mcces->chrs, i = v->mcces->nchrs; i > 0; p++, i--)
	{
		co = GETCOLOR(v->cm, *p);
		a = findarc(lp, PLAIN, co);
		ba = findarc(left, PLAIN, co);
		if (ba == NULL)
		{
			assert(a != NULL);
			freearc(v->nfa, a);
		}
		else
			assert(a == NULL);
		s = newstate(v->nfa);
		NOERR();
		newarc(v->nfa, PLAIN, co, lp, s);
		NOERR();
		pa = findarc(v->mccepbegin, PLAIN, co);
		assert(pa != NULL);
		if (ba == NULL)
		{						/* easy case, need all of them */
			cloneouts(v->nfa, pa->to, s, rp, PLAIN);
			newarc(v->nfa, '$', 1, s, rp);
			newarc(v->nfa, '$', 0, s, rp);
			colorcomplement(v->nfa, v->cm, AHEAD, pa->to, s, rp);
		}
		else
		{						/* must be selective */
			if (findarc(ba->to, '$', 1) == NULL)
			{
				newarc(v->nfa, '$', 1, s, rp);
				newarc(v->nfa, '$', 0, s, rp);
				colorcomplement(v->nfa, v->cm, AHEAD, pa->to,
								s, rp);
			}
			for (pa = pa->to->outs; pa != NULL; pa = pa->outchain)
				if (findarc(ba->to, PLAIN, pa->co) == NULL)
					newarc(v->nfa, PLAIN, pa->co, s, rp);
			if (s->nouts == 0)	/* limit of selectivity: none */
				dropstate(v->nfa, s);	/* frees arc too */
		}
		NOERR();
	}

	delsub(v->nfa, left, right);
	assert(left->nouts == 0);
	freestate(v->nfa, left);
	assert(right->nins == 0);
	freestate(v->nfa, right);
}

/*
 * brackpart - handle one item (or range) within a bracket expression
 */
static void
brackpart(struct vars * v,
		  struct state * lp,
		  struct state * rp)
{
	celt		startc;
	celt		endc;
	struct cvec *cv;
	chr		   *startp;
	chr		   *endp;
	chr			c[1];

	/* parse something, get rid of special cases, take shortcuts */
	switch (v->nexttype)
	{
		case RANGE:				/* a-b-c or other botch */
			ERR(REG_ERANGE);
			return;
			break;
		case PLAIN:
			c[0] = v->nextvalue;
			NEXT();
			/* shortcut for ordinary chr (not range, not MCCE leader) */
			if (!SEE(RANGE) && !ISCELEADER(v, c[0]))
			{
				onechr(v, c[0], lp, rp);
				return;
			}
			startc = element(v, c, c + 1);
			NOERR();
			break;
		case COLLEL:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECOLLATE);
			NOERR();
			startc = element(v, startp, endp);
			NOERR();
			break;
		case ECLASS:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECOLLATE);
			NOERR();
			startc = element(v, startp, endp);
			NOERR();
			cv = eclass(v, startc, (v->cflags & REG_ICASE));
			NOERR();
			dovec(v, cv, lp, rp);
			return;
			break;
		case CCLASS:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECTYPE);
			NOERR();
			cv = cclass(v, startp, endp, (v->cflags & REG_ICASE));
			NOERR();
			dovec(v, cv, lp, rp);
			return;
			break;
		default:
			ERR(REG_ASSERT);
			return;
			break;
	}

	if (SEE(RANGE))
	{
		NEXT();
		switch (v->nexttype)
		{
			case PLAIN:
			case RANGE:
				c[0] = v->nextvalue;
				NEXT();
				endc = element(v, c, c + 1);
				NOERR();
				break;
			case COLLEL:
				startp = v->now;
				endp = scanplain(v);
				INSIST(startp < endp, REG_ECOLLATE);
				NOERR();
				endc = element(v, startp, endp);
				NOERR();
				break;
			default:
				ERR(REG_ERANGE);
				return;
				break;
		}
	}
	else
		endc = startc;

	/*
	 * Ranges are unportable.  Actually, standard C does guarantee that digits
	 * are contiguous, but making that an exception is just too complicated.
	 */
	if (startc != endc)
		NOTE(REG_UUNPORT);
	cv = range(v, startc, endc, (v->cflags & REG_ICASE));
	NOERR();
	dovec(v, cv, lp, rp);
}

/*
 * scanplain - scan PLAIN contents of [. etc.
 *
 * Certain bits of trickery in lex.c know that this code does not try
 * to look past the final bracket of the [. etc.
 */
static chr *					/* just after end of sequence */
scanplain(struct vars * v)
{
	chr		   *endp;

	assert(SEE(COLLEL) || SEE(ECLASS) || SEE(CCLASS));
	NEXT();

	endp = v->now;
	while (SEE(PLAIN))
	{
		endp = v->now;
		NEXT();
	}

	assert(SEE(END) || ISERR());
	NEXT();

	return endp;
}

/*
 * leaders - process a cvec of collating elements to also include leaders
 * Also gives all characters involved their own colors, which is almost
 * certainly necessary, and sets up little disconnected subNFA.
 */
static void
leaders(struct vars * v,
		struct cvec * cv)
{
	int			mcce;
	chr		   *p;
	chr			leader;
	struct state *s;
	struct arc *a;

	v->mccepbegin = newstate(v->nfa);
	v->mccepend = newstate(v->nfa);
	NOERR();

	for (mcce = 0; mcce < cv->nmcces; mcce++)
	{
		p = cv->mcces[mcce];
		leader = *p;
		if (!haschr(cv, leader))
		{
			addchr(cv, leader);
			s = newstate(v->nfa);
			newarc(v->nfa, PLAIN, subcolor(v->cm, leader),
				   v->mccepbegin, s);
			okcolors(v->nfa, v->cm);
		}
		else
		{
			a = findarc(v->mccepbegin, PLAIN,
						GETCOLOR(v->cm, leader));
			assert(a != NULL);
			s = a->to;
			assert(s != v->mccepend);
		}
		p++;
		assert(*p != 0 && *(p + 1) == 0);		/* only 2-char MCCEs for now */
		newarc(v->nfa, PLAIN, subcolor(v->cm, *p), s, v->mccepend);
		okcolors(v->nfa, v->cm);
	}
}

/*
 * onechr - fill in arcs for a plain character, and possible case complements
 * This is mostly a shortcut for efficient handling of the common case.
 */
static void
onechr(struct vars * v,
	   chr c,
	   struct state * lp,
	   struct state * rp)
{
	if (!(v->cflags & REG_ICASE))
	{
		newarc(v->nfa, PLAIN, subcolor(v->cm, c), lp, rp);
		return;
	}

	/* rats, need general case anyway... */
	dovec(v, allcases(v, c), lp, rp);
}

/*
 * dovec - fill in arcs for each element of a cvec
 * This one has to handle the messy cases, like MCCEs and MCCE leaders.
 */
static void
dovec(struct vars * v,
	  struct cvec * cv,
	  struct state * lp,
	  struct state * rp)
{
	chr			ch,
				from,
				to;
	celt		ce;
	chr		   *p;
	int			i;
	color		co;
	struct cvec *leads;
	struct arc *a;
	struct arc *pa;				/* arc in prototype */
	struct state *s;
	struct state *ps;			/* state in prototype */

	/* need a place to store leaders, if any */
	if (nmcces(v) > 0)
	{
		assert(v->mcces != NULL);
		if (v->cv2 == NULL || v->cv2->nchrs < v->mcces->nchrs)
		{
			if (v->cv2 != NULL)
				free(v->cv2);
			v->cv2 = newcvec(v->mcces->nchrs, 0, v->mcces->nmcces);
			NOERR();
			leads = v->cv2;
		}
		else
			leads = clearcvec(v->cv2);
	}
	else
		leads = NULL;

	/* first, get the ordinary characters out of the way */
	for (p = cv->chrs, i = cv->nchrs; i > 0; p++, i--)
	{
		ch = *p;
		if (!ISCELEADER(v, ch))
			newarc(v->nfa, PLAIN, subcolor(v->cm, ch), lp, rp);
		else
		{
			assert(singleton(v->cm, ch));
			assert(leads != NULL);
			if (!haschr(leads, ch))
				addchr(leads, ch);
		}
	}

	/* and the ranges */
	for (p = cv->ranges, i = cv->nranges; i > 0; p += 2, i--)
	{
		from = *p;
		to = *(p + 1);
		while (from <= to && (ce = nextleader(v, from, to)) != NOCELT)
		{
			if (from < ce)
				subrange(v, from, ce - 1, lp, rp);
			assert(singleton(v->cm, ce));
			assert(leads != NULL);
			if (!haschr(leads, ce))
				addchr(leads, ce);
			from = ce + 1;
		}
		if (from <= to)
			subrange(v, from, to, lp, rp);
	}

	if ((leads == NULL || leads->nchrs == 0) && cv->nmcces == 0)
		return;

	/* deal with the MCCE leaders */
	NOTE(REG_ULOCALE);
	for (p = leads->chrs, i = leads->nchrs; i > 0; p++, i--)
	{
		co = GETCOLOR(v->cm, *p);
		a = findarc(lp, PLAIN, co);
		if (a != NULL)
			s = a->to;
		else
		{
			s = newstate(v->nfa);
			NOERR();
			newarc(v->nfa, PLAIN, co, lp, s);
			NOERR();
		}
		pa = findarc(v->mccepbegin, PLAIN, co);
		assert(pa != NULL);
		ps = pa->to;
		newarc(v->nfa, '$', 1, s, rp);
		newarc(v->nfa, '$', 0, s, rp);
		colorcomplement(v->nfa, v->cm, AHEAD, ps, s, rp);
		NOERR();
	}

	/* and the MCCEs */
	for (i = 0; i < cv->nmcces; i++)
	{
		p = cv->mcces[i];
		assert(singleton(v->cm, *p));
		if (!singleton(v->cm, *p))
		{
			ERR(REG_ASSERT);
			return;
		}
		ch = *p++;
		co = GETCOLOR(v->cm, ch);
		a = findarc(lp, PLAIN, co);
		if (a != NULL)
			s = a->to;
		else
		{
			s = newstate(v->nfa);
			NOERR();
			newarc(v->nfa, PLAIN, co, lp, s);
			NOERR();
		}
		assert(*p != 0);		/* at least two chars */
		assert(singleton(v->cm, *p));
		ch = *p++;
		co = GETCOLOR(v->cm, ch);
		assert(*p == 0);		/* and only two, for now */
		newarc(v->nfa, PLAIN, co, s, rp);
		NOERR();
	}
}

/*
 * nextleader - find next MCCE leader within range
 */
static celt						/* NOCELT means none */
nextleader(struct vars * v,
		   chr from,
		   chr to)
{
	int			i;
	chr		   *p;
	chr			ch;
	celt		it = NOCELT;

	if (v->mcces == NULL)
		return it;

	for (i = v->mcces->nchrs, p = v->mcces->chrs; i > 0; i--, p++)
	{
		ch = *p;
		if (from <= ch && ch <= to)
			if (it == NOCELT || ch < it)
				it = ch;
	}
	return it;
}

/*
 * wordchrs - set up word-chr list for word-boundary stuff, if needed
 *
 * The list is kept as a bunch of arcs between two dummy states; it's
 * disposed of by the unreachable-states sweep in NFA optimization.
 * Does NEXT().  Must not be called from any unusual lexical context.
 * This should be reconciled with the \w etc. handling in lex.c, and
 * should be cleaned up to reduce dependencies on input scanning.
 */
static void
wordchrs(struct vars * v)
{
	struct state *left;
	struct state *right;

	if (v->wordchrs != NULL)
	{
		NEXT();					/* for consistency */
		return;
	}

	left = newstate(v->nfa);
	right = newstate(v->nfa);
	NOERR();
	/* fine point:	implemented with [::], and lexer will set REG_ULOCALE */
	lexword(v);
	NEXT();
	assert(v->savenow != NULL && SEE('['));
	bracket(v, left, right);
	assert((v->savenow != NULL && SEE(']')) || ISERR());
	NEXT();
	NOERR();
	v->wordchrs = left;
}

/*
 * subre - allocate a subre
 */
static struct subre *
subre(struct vars * v,
	  int op,
	  int flags,
	  struct state * begin,
	  struct state * end)
{
	struct subre *ret;

	ret = v->treefree;
	if (ret != NULL)
		v->treefree = ret->left;
	else
	{
		ret = (struct subre *) MALLOC(sizeof(struct subre));
		if (ret == NULL)
		{
			ERR(REG_ESPACE);
			return NULL;
		}
		ret->chain = v->treechain;
		v->treechain = ret;
	}

	assert(strchr("|.b(=", op) != NULL);

	ret->op = op;
	ret->flags = flags;
	ret->retry = 0;
	ret->subno = 0;
	ret->min = ret->max = 1;
	ret->left = NULL;
	ret->right = NULL;
	ret->begin = begin;
	ret->end = end;
	ZAPCNFA(ret->cnfa);

	return ret;
}

/*
 * freesubre - free a subRE subtree
 */
static void
freesubre(struct vars * v,		/* might be NULL */
		  struct subre * sr)
{
	if (sr == NULL)
		return;

	if (sr->left != NULL)
		freesubre(v, sr->left);
	if (sr->right != NULL)
		freesubre(v, sr->right);

	freesrnode(v, sr);
}

/*
 * freesrnode - free one node in a subRE subtree
 */
static void
freesrnode(struct vars * v,		/* might be NULL */
		   struct subre * sr)
{
	if (sr == NULL)
		return;

	if (!NULLCNFA(sr->cnfa))
		freecnfa(&sr->cnfa);
	sr->flags = 0;

	if (v != NULL)
	{
		sr->left = v->treefree;
		v->treefree = sr;
	}
	else
		FREE(sr);
}

/*
 * optst - optimize a subRE subtree
 */
static void
optst(struct vars * v,
	  struct subre * t)
{
	if (t == NULL)
		return;

	/* recurse through children */
	if (t->left != NULL)
		optst(v, t->left);
	if (t->right != NULL)
		optst(v, t->right);
}

/*
 * numst - number tree nodes (assigning retry indexes)
 */
static int						/* next number */
numst(struct subre * t,
	  int start)				/* starting point for subtree numbers */
{
	int			i;

	assert(t != NULL);

	i = start;
	t->retry = (short) i++;
	if (t->left != NULL)
		i = numst(t->left, i);
	if (t->right != NULL)
		i = numst(t->right, i);
	return i;
}

/*
 * markst - mark tree nodes as INUSE
 */
static void
markst(struct subre * t)
{
	assert(t != NULL);

	t->flags |= INUSE;
	if (t->left != NULL)
		markst(t->left);
	if (t->right != NULL)
		markst(t->right);
}

/*
 * cleanst - free any tree nodes not marked INUSE
 */
static void
cleanst(struct vars * v)
{
	struct subre *t;
	struct subre *next;

	for (t = v->treechain; t != NULL; t = next)
	{
		next = t->chain;
		if (!(t->flags & INUSE))
			FREE(t);
	}
	v->treechain = NULL;
	v->treefree = NULL;			/* just on general principles */
}

/*
 * nfatree - turn a subRE subtree into a tree of compacted NFAs
 */
static long						/* optimize results from top node */
nfatree(struct vars * v,
		struct subre * t,
		FILE *f)				/* for debug output */
{
	assert(t != NULL && t->begin != NULL);

	if (t->left != NULL)
		(DISCARD) nfatree(v, t->left, f);
	if (t->right != NULL)
		(DISCARD) nfatree(v, t->right, f);

	return nfanode(v, t, f);
}

/*
 * nfanode - do one NFA for nfatree
 */
static long						/* optimize results */
nfanode(struct vars * v,
		struct subre * t,
		FILE *f)				/* for debug output */
{
	struct nfa *nfa;
	long		ret = 0;

	assert(t->begin != NULL);

#ifdef REG_DEBUG
	if (f != NULL)
	{
		char		idbuf[50];

		fprintf(f, "\n\n\n========= TREE NODE %s ==========\n",
				stid(t, idbuf, sizeof(idbuf)));
	}
#endif
	nfa = newnfa(v, v->cm, v->nfa);
	NOERRZ();
	dupnfa(nfa, t->begin, t->end, nfa->init, nfa->final);
	if (!ISERR())
	{
		specialcolors(nfa);
		ret = optimize(nfa, f);
	}
	if (!ISERR())
		compact(nfa, &t->cnfa);

	freenfa(nfa);
	return ret;
}

/*
 * newlacon - allocate a lookahead-constraint subRE
 */
static int						/* lacon number */
newlacon(struct vars * v,
		 struct state * begin,
		 struct state * end,
		 int pos)
{
	int			n;
	struct subre *sub;

	if (v->nlacons == 0)
	{
		v->lacons = (struct subre *) MALLOC(2 * sizeof(struct subre));
		n = 1;					/* skip 0th */
		v->nlacons = 2;
	}
	else
	{
		v->lacons = (struct subre *) REALLOC(v->lacons,
									(v->nlacons + 1) * sizeof(struct subre));
		n = v->nlacons++;
	}
	if (v->lacons == NULL)
	{
		ERR(REG_ESPACE);
		return 0;
	}
	sub = &v->lacons[n];
	sub->begin = begin;
	sub->end = end;
	sub->subno = pos;
	ZAPCNFA(sub->cnfa);
	return n;
}

/*
 * freelacons - free lookahead-constraint subRE vector
 */
static void
freelacons(struct subre * subs,
		   int n)
{
	struct subre *sub;
	int			i;

	assert(n > 0);
	for (sub = subs + 1, i = n - 1; i > 0; sub++, i--)	/* no 0th */
		if (!NULLCNFA(sub->cnfa))
			freecnfa(&sub->cnfa);
	FREE(subs);
}

/*
 * rfree - free a whole RE (insides of regfree)
 */
static void
rfree(regex_t *re)
{
	struct guts *g;

	if (re == NULL || re->re_magic != REMAGIC)
		return;

	re->re_magic = 0;			/* invalidate RE */
	g = (struct guts *) re->re_guts;
	re->re_guts = NULL;
	re->re_fns = NULL;
	g->magic = 0;
	freecm(&g->cmap);
	if (g->tree != NULL)
		freesubre((struct vars *) NULL, g->tree);
	if (g->lacons != NULL)
		freelacons(g->lacons, g->nlacons);
	if (!NULLCNFA(g->search))
		freecnfa(&g->search);
	FREE(g);
}

#ifdef REG_DEBUG

/*
 * dump - dump an RE in human-readable form
 */
static void
dump(regex_t *re,
	 FILE *f)
{
	struct guts *g;
	int			i;

	if (re->re_magic != REMAGIC)
		fprintf(f, "bad magic number (0x%x not 0x%x)\n", re->re_magic,
				REMAGIC);
	if (re->re_guts == NULL)
	{
		fprintf(f, "NULL guts!!!\n");
		return;
	}
	g = (struct guts *) re->re_guts;
	if (g->magic != GUTSMAGIC)
		fprintf(f, "bad guts magic number (0x%x not 0x%x)\n", g->magic,
				GUTSMAGIC);

	fprintf(f, "\n\n\n========= DUMP ==========\n");
	fprintf(f, "nsub %d, info 0%lo, csize %d, ntree %d\n",
			re->re_nsub, re->re_info, re->re_csize, g->ntree);

	dumpcolors(&g->cmap, f);
	if (!NULLCNFA(g->search))
	{
		printf("\nsearch:\n");
		dumpcnfa(&g->search, f);
	}
	for (i = 1; i < g->nlacons; i++)
	{
		fprintf(f, "\nla%d (%s):\n", i,
				(g->lacons[i].subno) ? "positive" : "negative");
		dumpcnfa(&g->lacons[i].cnfa, f);
	}
	fprintf(f, "\n");
	dumpst(g->tree, f, 0);
}

/*
 * dumpst - dump a subRE tree
 */
static void
dumpst(struct subre * t,
	   FILE *f,
	   int nfapresent)			/* is the original NFA still around? */
{
	if (t == NULL)
		fprintf(f, "null tree\n");
	else
		stdump(t, f, nfapresent);
	fflush(f);
}

/*
 * stdump - recursive guts of dumpst
 */
static void
stdump(struct subre * t,
	   FILE *f,
	   int nfapresent)			/* is the original NFA still around? */
{
	char		idbuf[50];

	fprintf(f, "%s. `%c'", stid(t, idbuf, sizeof(idbuf)), t->op);
	if (t->flags & LONGER)
		fprintf(f, " longest");
	if (t->flags & SHORTER)
		fprintf(f, " shortest");
	if (t->flags & MIXED)
		fprintf(f, " hasmixed");
	if (t->flags & CAP)
		fprintf(f, " hascapture");
	if (t->flags & BACKR)
		fprintf(f, " hasbackref");
	if (!(t->flags & INUSE))
		fprintf(f, " UNUSED");
	if (t->subno != 0)
		fprintf(f, " (#%d)", t->subno);
	if (t->min != 1 || t->max != 1)
	{
		fprintf(f, " {%d,", t->min);
		if (t->max != INFINITY)
			fprintf(f, "%d", t->max);
		fprintf(f, "}");
	}
	if (nfapresent)
		fprintf(f, " %ld-%ld", (long) t->begin->no, (long) t->end->no);
	if (t->left != NULL)
		fprintf(f, " L:%s", stid(t->left, idbuf, sizeof(idbuf)));
	if (t->right != NULL)
		fprintf(f, " R:%s", stid(t->right, idbuf, sizeof(idbuf)));
	if (!NULLCNFA(t->cnfa))
	{
		fprintf(f, "\n");
		dumpcnfa(&t->cnfa, f);
		fprintf(f, "\n");
	}
	if (t->left != NULL)
		stdump(t->left, f, nfapresent);
	if (t->right != NULL)
		stdump(t->right, f, nfapresent);
}

/*
 * stid - identify a subtree node for dumping
 */
static char *					/* points to buf or constant string */
stid(struct subre * t,
	 char *buf,
	 size_t bufsize)
{
	/* big enough for hex int or decimal t->retry? */
	if (bufsize < sizeof(void *) * 2 + 3 || bufsize < sizeof(t->retry) * 3 + 1)
		return "unable";
	if (t->retry != 0)
		sprintf(buf, "%d", t->retry);
	else
		sprintf(buf, "%p", t);
	return buf;
}
#endif   /* REG_DEBUG */


#include "regc_lex.c"
#include "regc_color.c"
#include "regc_nfa.c"
#include "regc_cvec.c"
#include "regc_locale.c"
