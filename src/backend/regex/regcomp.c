/*
 * re_*comp and friends - compile REs
 * This file #includes several others (see the bottom).
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
 * src/backend/regex/regcomp.c
 *
 */

#include "regex/regguts.h"

/*
 * forward declarations, up here so forward datatypes etc. are defined early
 */
/* === regcomp.c === */
static void moresubs(struct vars *v, int wanted);
static int	freev(struct vars *v, int err);
static void makesearch(struct vars *v, struct nfa *nfa);
static struct subre *parse(struct vars *v, int stopper, int type,
						   struct state *init, struct state *final);
static struct subre *parsebranch(struct vars *v, int stopper, int type,
								 struct state *left, struct state *right,
								 int partial);
static struct subre *parseqatom(struct vars *v, int stopper, int type,
								struct state *lp, struct state *rp,
								struct subre *top);
static void nonword(struct vars *v, int dir, struct state *lp,
					struct state *rp);
static void word(struct vars *v, int dir, struct state *lp, struct state *rp);
static void charclass(struct vars *v, enum char_classes cls, struct state *lp,
					  struct state *rp);
static void charclasscomplement(struct vars *v, enum char_classes cls,
								struct state *lp, struct state *rp);
static int	scannum(struct vars *v);
static void repeat(struct vars *v, struct state *lp, struct state *rp,
				   int m, int n);
static void bracket(struct vars *v, struct state *lp, struct state *rp);
static void cbracket(struct vars *v, struct state *lp, struct state *rp);
static void brackpart(struct vars *v, struct state *lp, struct state *rp,
					  bool *have_cclassc);
static const chr *scanplain(struct vars *v);
static void onechr(struct vars *v, chr c, struct state *lp, struct state *rp);
static void optimizebracket(struct vars *v, struct state *lp, struct state *rp);
static void wordchrs(struct vars *v);
static void processlacon(struct vars *v, struct state *begin,
						 struct state *end, int latype,
						 struct state *lp, struct state *rp);
static struct subre *subre(struct vars *v, int op, int flags,
						   struct state *begin, struct state *end);
static void freesubre(struct vars *v, struct subre *sr);
static void freesubreandsiblings(struct vars *v, struct subre *sr);
static void freesrnode(struct vars *v, struct subre *sr);
static void removecaptures(struct vars *v, struct subre *t);
static int	numst(struct subre *t, int start);
static void markst(struct subre *t);
static void cleanst(struct vars *v);
static long nfatree(struct vars *v, struct subre *t, FILE *f);
static long nfanode(struct vars *v, struct subre *t,
					int converttosearch, FILE *f);
static int	newlacon(struct vars *v, struct state *begin, struct state *end,
					 int latype);
static void freelacons(struct subre *subs, int n);
static void rfree(regex_t *re);
static int	rstacktoodeep(void);

#ifdef REG_DEBUG
static void dump(regex_t *re, FILE *f);
static void dumpst(struct subre *t, FILE *f, int nfapresent);
static void stdump(struct subre *t, FILE *f, int nfapresent);
static const char *stid(struct subre *t, char *buf, size_t bufsize);
#endif
/* === regc_lex.c === */
static void lexstart(struct vars *v);
static void prefixes(struct vars *v);
static int	next(struct vars *v);
static int	lexescape(struct vars *v);
static chr	lexdigits(struct vars *v, int base, int minlen, int maxlen);
static int	brenext(struct vars *v, chr c);
static void skip(struct vars *v);
static chr	newline(void);
static chr	chrnamed(struct vars *v, const chr *startp, const chr *endp,
					 chr lastresort);

/* === regc_color.c === */
static void initcm(struct vars *v, struct colormap *cm);
static void freecm(struct colormap *cm);
static color maxcolor(struct colormap *cm);
static color newcolor(struct colormap *cm);
static void freecolor(struct colormap *cm, color co);
static color pseudocolor(struct colormap *cm);
static color subcolor(struct colormap *cm, chr c);
static color subcolorhi(struct colormap *cm, color *pco);
static color newsub(struct colormap *cm, color co);
static int	newhicolorrow(struct colormap *cm, int oldrow);
static void newhicolorcols(struct colormap *cm);
static void subcolorcvec(struct vars *v, struct cvec *cv, struct state *lp,
						 struct state *rp);
static void subcoloronechr(struct vars *v, chr ch, struct state *lp,
						   struct state *rp, color *lastsubcolor);
static void subcoloronerange(struct vars *v, chr from, chr to,
							 struct state *lp, struct state *rp,
							 color *lastsubcolor);
static void subcoloronerow(struct vars *v, int rownum, struct state *lp,
						   struct state *rp, color *lastsubcolor);
static void okcolors(struct nfa *nfa, struct colormap *cm);
static void colorchain(struct colormap *cm, struct arc *a);
static void uncolorchain(struct colormap *cm, struct arc *a);
static void rainbow(struct nfa *nfa, struct colormap *cm, int type, color but,
					struct state *from, struct state *to);
static void colorcomplement(struct nfa *nfa, struct colormap *cm, int type,
							struct state *of, struct state *from,
							struct state *to);

#ifdef REG_DEBUG
static void dumpcolors(struct colormap *cm, FILE *f);
static void dumpchr(chr c, FILE *f);
#endif
/* === regc_nfa.c === */
static struct nfa *newnfa(struct vars *v, struct colormap *cm,
						  struct nfa *parent);
static void freenfa(struct nfa *nfa);
static struct state *newstate(struct nfa *nfa);
static struct state *newfstate(struct nfa *nfa, int flag);
static void dropstate(struct nfa *nfa, struct state *s);
static void freestate(struct nfa *nfa, struct state *s);
static void newarc(struct nfa *nfa, int t, color co,
				   struct state *from, struct state *to);
static void createarc(struct nfa *nfa, int t, color co,
					  struct state *from, struct state *to);
static struct arc *allocarc(struct nfa *nfa);
static void freearc(struct nfa *nfa, struct arc *victim);
static void changearcsource(struct arc *a, struct state *newfrom);
static void changearctarget(struct arc *a, struct state *newto);
static int	hasnonemptyout(struct state *s);
static struct arc *findarc(struct state *s, int type, color co);
static void cparc(struct nfa *nfa, struct arc *oa,
				  struct state *from, struct state *to);
static void sortins(struct nfa *nfa, struct state *s);
static int	sortins_cmp(const void *a, const void *b);
static void sortouts(struct nfa *nfa, struct state *s);
static int	sortouts_cmp(const void *a, const void *b);
static void moveins(struct nfa *nfa, struct state *oldState,
					struct state *newState);
static void copyins(struct nfa *nfa, struct state *oldState,
					struct state *newState);
static void mergeins(struct nfa *nfa, struct state *s,
					 struct arc **arcarray, int arccount);
static void moveouts(struct nfa *nfa, struct state *oldState,
					 struct state *newState);
static void copyouts(struct nfa *nfa, struct state *oldState,
					 struct state *newState);
static void cloneouts(struct nfa *nfa, struct state *old, struct state *from,
					  struct state *to, int type);
static void delsub(struct nfa *nfa, struct state *lp, struct state *rp);
static void deltraverse(struct nfa *nfa, struct state *leftend,
						struct state *s);
static void dupnfa(struct nfa *nfa, struct state *start, struct state *stop,
				   struct state *from, struct state *to);
static void duptraverse(struct nfa *nfa, struct state *s, struct state *stmp);
static void removeconstraints(struct nfa *nfa, struct state *start, struct state *stop);
static void removetraverse(struct nfa *nfa, struct state *s);
static void cleartraverse(struct nfa *nfa, struct state *s);
static struct state *single_color_transition(struct state *s1,
											 struct state *s2);
static void specialcolors(struct nfa *nfa);
static long optimize(struct nfa *nfa, FILE *f);
static void pullback(struct nfa *nfa, FILE *f);
static int	pull(struct nfa *nfa, struct arc *con,
				 struct state **intermediates);
static void pushfwd(struct nfa *nfa, FILE *f);
static int	push(struct nfa *nfa, struct arc *con,
				 struct state **intermediates);

#define INCOMPATIBLE	1		/* destroys arc */
#define SATISFIED	2			/* constraint satisfied */
#define COMPATIBLE	3			/* compatible but not satisfied yet */
#define REPLACEARC	4			/* replace arc's color with constraint color */
static int	combine(struct nfa *nfa, struct arc *con, struct arc *a);
static void fixempties(struct nfa *nfa, FILE *f);
static struct state *emptyreachable(struct nfa *nfa, struct state *s,
									struct state *lastfound,
									struct arc **inarcsorig);
static int	isconstraintarc(struct arc *a);
static int	hasconstraintout(struct state *s);
static void fixconstraintloops(struct nfa *nfa, FILE *f);
static int	findconstraintloop(struct nfa *nfa, struct state *s);
static void breakconstraintloop(struct nfa *nfa, struct state *sinitial);
static void clonesuccessorstates(struct nfa *nfa, struct state *ssource,
								 struct state *sclone,
								 struct state *spredecessor,
								 struct arc *refarc, char *curdonemap,
								 char *outerdonemap, int nstates);
static void removecantmatch(struct nfa *nfa);
static void cleanup(struct nfa *nfa);
static void markreachable(struct nfa *nfa, struct state *s,
						  struct state *okay, struct state *mark);
static void markcanreach(struct nfa *nfa, struct state *s, struct state *okay,
						 struct state *mark);
static long analyze(struct nfa *nfa);
static void checkmatchall(struct nfa *nfa);
static bool checkmatchall_recurse(struct nfa *nfa, struct state *s,
								  bool **haspaths);
static bool check_out_colors_match(struct state *s, color co1, color co2);
static bool check_in_colors_match(struct state *s, color co1, color co2);
static void compact(struct nfa *nfa, struct cnfa *cnfa);
static void carcsort(struct carc *first, size_t n);
static int	carc_cmp(const void *a, const void *b);
static void freecnfa(struct cnfa *cnfa);
static void dumpnfa(struct nfa *nfa, FILE *f);

#ifdef REG_DEBUG
static void dumpstate(struct state *s, FILE *f);
static void dumparcs(struct state *s, FILE *f);
static void dumparc(struct arc *a, struct state *s, FILE *f);
static void dumpcnfa(struct cnfa *cnfa, FILE *f);
static void dumpcstate(int st, struct cnfa *cnfa, FILE *f);
#endif
/* === regc_cvec.c === */
static struct cvec *newcvec(int nchrs, int nranges);
static struct cvec *clearcvec(struct cvec *cv);
static void addchr(struct cvec *cv, chr c);
static void addrange(struct cvec *cv, chr from, chr to);
static struct cvec *getcvec(struct vars *v, int nchrs, int nranges);
static void freecvec(struct cvec *cv);

/* === regc_pg_locale.c === */
static int	pg_wc_isdigit(pg_wchar c);
static int	pg_wc_isalpha(pg_wchar c);
static int	pg_wc_isalnum(pg_wchar c);
static int	pg_wc_isword(pg_wchar c);
static int	pg_wc_isupper(pg_wchar c);
static int	pg_wc_islower(pg_wchar c);
static int	pg_wc_isgraph(pg_wchar c);
static int	pg_wc_isprint(pg_wchar c);
static int	pg_wc_ispunct(pg_wchar c);
static int	pg_wc_isspace(pg_wchar c);
static pg_wchar pg_wc_toupper(pg_wchar c);
static pg_wchar pg_wc_tolower(pg_wchar c);

/* === regc_locale.c === */
static chr	element(struct vars *v, const chr *startp, const chr *endp);
static struct cvec *range(struct vars *v, chr a, chr b, int cases);
static int	before(chr x, chr y);
static struct cvec *eclass(struct vars *v, chr c, int cases);
static enum char_classes lookupcclass(struct vars *v, const chr *startp,
									  const chr *endp);
static struct cvec *cclasscvec(struct vars *v, enum char_classes cclasscode,
							   int cases);
static int	cclass_column_index(struct colormap *cm, chr c);
static struct cvec *allcases(struct vars *v, chr c);
static int	cmp(const chr *x, const chr *y, size_t len);
static int	casecmp(const chr *x, const chr *y, size_t len);


/* internal variables, bundled for easy passing around */
struct vars
{
	regex_t    *re;
	const chr  *now;			/* scan pointer into string */
	const chr  *stop;			/* end of string */
	int			err;			/* error code (0 if none) */
	int			cflags;			/* copy of compile flags */
	int			lasttype;		/* type of previous token */
	int			nexttype;		/* type of next token */
	chr			nextvalue;		/* value (if any) of next token */
	int			lexcon;			/* lexical context type (see regc_lex.c) */
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
	int			ntree;			/* number of tree nodes, plus one */
	struct cvec *cv;			/* interface cvec */
	struct cvec *cv2;			/* utility cvec */
	struct subre *lacons;		/* lookaround-constraint vector */
	int			nlacons;		/* size of lacons[]; note that only slots
								 * numbered 1 .. nlacons-1 are used */
	size_t		spaceused;		/* approx. space used for compilation */
};

/* parsing macros; most know that `v' is the struct vars pointer */
#define NEXT()	(next(v))		/* advance by one token */
#define SEE(t)	(v->nexttype == (t))	/* is next token this? */
#define EAT(t)	(SEE(t) && next(v)) /* if next is this, swallow it */
#define VISERR(vv)	((vv)->err != 0)	/* have we seen an error yet? */
#define ISERR() VISERR(v)
#define VERR(vv,e)	((vv)->nexttype = EOS, \
					 (vv)->err = ((vv)->err ? (vv)->err : (e)))
#define ERR(e)	VERR(v, e)		/* record an error */
#define NOERR() {if (ISERR()) return;}	/* if error seen, return */
#define NOERRN()	{if (ISERR()) return NULL;} /* NOERR with retval */
#define NOERRZ()	{if (ISERR()) return 0;}	/* NOERR with retval */
#define INSIST(c, e) do { if (!(c)) ERR(e); } while (0) /* error if c false */
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
#define CCLASSS	's'				/* char class shorthand escape */
#define CCLASSC	'c'				/* complement char class shorthand escape */
#define RANGE	'R'				/* - within [] which might be range delim. */
#define LACON	'L'				/* lookaround constraint subRE */
#define AHEAD	'a'				/* color-lookahead arc */
#define BEHIND	'r'				/* color-lookbehind arc */
#define WBDRY	'w'				/* word boundary constraint */
#define NWBDRY	'W'				/* non-word-boundary constraint */
#define CANTMATCH 'x'			/* arc that cannot match anything */
#define SBEGIN	'A'				/* beginning of string (even if not BOL) */
#define SEND	'Z'				/* end of string (even if not EOL) */

/* is an arc colored, and hence should belong to a color chain? */
/* the test on "co" eliminates RAINBOW arcs, which we don't bother to chain */
#define COLORED(a) \
	((a)->co >= 0 && \
	 ((a)->type == PLAIN || (a)->type == AHEAD || (a)->type == BEHIND))


/* static function list */
static const struct fns functions = {
	rfree,						/* regfree insides */
	rstacktoodeep				/* check for stack getting dangerously deep */
};



/*
 * pg_regcomp - compile regular expression
 *
 * Note: on failure, no resources remain allocated, so pg_regfree()
 * need not be applied to re.
 */
int
pg_regcomp(regex_t *re,
		   const chr *string,
		   size_t len,
		   int flags,
		   Oid collation)
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

	/* Initialize locale-dependent support */
	pg_set_regex_collation(collation);

	/* initial setup (after which freev() is callable) */
	v->re = re;
	v->now = string;
	v->stop = v->now + len;
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
	v->lacons = NULL;
	v->nlacons = 0;
	v->spaceused = 0;
	re->re_magic = REMAGIC;
	re->re_info = 0;			/* bits get set during parse */
	re->re_csize = sizeof(chr);
	re->re_collation = collation;
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
	/* set up a reasonably-sized transient cvec for getcvec usage */
	v->cv = newcvec(100, 20);
	if (v->cv == NULL)
		return freev(v, REG_ESPACE);

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
	if (v->cflags & REG_NOSUB)
		removecaptures(v, v->tree);
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
		struct subre *lasub = &v->lacons[i];

#ifdef REG_DEBUG
		if (debug != NULL)
			fprintf(debug, "\n\n\n========= LA%d ==========\n", i);
#endif

		/* Prepend .* to pattern if it's a lookbehind LACON */
		nfanode(v, lasub, !LATYPE_IS_AHEAD(lasub->latype), debug);
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
	{
		dump(re, stdout);
		fflush(stdout);
	}
#endif

	assert(v->err == 0);
	return freev(v, 0);
}

/*
 * moresubs - enlarge subRE vector
 */
static void
moresubs(struct vars *v,
		 int wanted)			/* want enough room for this one */
{
	struct subre **p;
	size_t		n;

	assert(wanted > 0 && (size_t) wanted >= v->nsubs);
	n = (size_t) wanted * 3 / 2 + 1;

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
freev(struct vars *v,
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
makesearch(struct vars *v,
		   struct nfa *nfa)
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

		/*
		 * The pattern is still MATCHALL if it was before, but the max match
		 * length is now infinity.
		 */
		if (nfa->flags & MATCHALL)
			nfa->maxmatchall = DUPINF;
	}

	/*
	 * Now here's the subtle part.  Because many REs have no lookback
	 * constraints, often knowing when you were in the pre state tells you
	 * little; it's the next state(s) that are informative.  But some of them
	 * may have other inarcs, i.e. it may be possible to make actual progress
	 * and then return to one of them.  We must de-optimize such cases,
	 * splitting each such state into progress and no-progress states.
	 */

	/* first, make a list of the states reachable from pre and elsewhere */
	slist = NULL;
	for (a = pre->outs; a != NULL; a = a->outchain)
	{
		s = a->to;
		for (b = s->ins; b != NULL; b = b->inchain)
		{
			if (b->from != pre)
				break;
		}

		/*
		 * We want to mark states as being in the list already by having non
		 * NULL tmp fields, but we can't just store the old slist value in tmp
		 * because that doesn't work for the first such state.  Instead, the
		 * first list entry gets its own address in tmp.
		 */
		if (b != NULL && s->tmp == NULL)
		{
			s->tmp = (slist != NULL) ? slist : s;
			slist = s;
		}
	}

	/* do the splits */
	for (s = slist; s != NULL; s = s2)
	{
		s2 = newstate(nfa);
		NOERR();
		copyouts(nfa, s, s2);
		NOERR();
		for (a = s->ins; a != NULL; a = b)
		{
			b = a->inchain;
			if (a->from != pre)
			{
				cparc(nfa, a, a->from, s2);
				freearc(nfa, a);
			}
		}
		s2 = (s->tmp != s) ? s->tmp : NULL;
		s->tmp = NULL;			/* clean up while we're at it */
	}
}

/*
 * parse - parse an RE
 *
 * This is actually just the top level, which parses a bunch of branches
 * tied together with '|'.  If there's more than one, they appear in the
 * tree as the children of a '|' subre.
 */
static struct subre *
parse(struct vars *v,
	  int stopper,				/* EOS or ')' */
	  int type,					/* LACON (lookaround subRE) or PLAIN */
	  struct state *init,		/* initial state */
	  struct state *final)		/* final state */
{
	struct subre *branches;		/* top level */
	struct subre *lastbranch;	/* latest branch */

	assert(stopper == ')' || stopper == EOS);

	branches = subre(v, '|', LONGER, init, final);
	NOERRN();
	lastbranch = NULL;
	do
	{							/* a branch */
		struct subre *branch;
		struct state *left;		/* scaffolding for branch */
		struct state *right;

		left = newstate(v->nfa);
		right = newstate(v->nfa);
		NOERRN();
		EMPTYARC(init, left);
		EMPTYARC(right, final);
		NOERRN();
		branch = parsebranch(v, stopper, type, left, right, 0);
		NOERRN();
		if (lastbranch)
			lastbranch->sibling = branch;
		else
			branches->child = branch;
		branches->flags |= UP(branches->flags | branch->flags);
		lastbranch = branch;
	} while (EAT('|'));
	assert(SEE(stopper) || SEE(EOS));

	if (!SEE(stopper))
	{
		assert(stopper == ')' && SEE(EOS));
		ERR(REG_EPAREN);
	}

	/* optimize out simple cases */
	if (lastbranch == branches->child)
	{							/* only one branch */
		assert(lastbranch->sibling == NULL);
		freesrnode(v, branches);
		branches = lastbranch;
	}
	else if (!MESSY(branches->flags))
	{							/* no interesting innards */
		freesubreandsiblings(v, branches->child);
		branches->child = NULL;
		branches->op = '=';
	}

	return branches;
}

/*
 * parsebranch - parse one branch of an RE
 *
 * This mostly manages concatenation, working closely with parseqatom().
 * Concatenated things are bundled up as much as possible, with separate
 * '.' nodes introduced only when necessary due to substructure.
 */
static struct subre *
parsebranch(struct vars *v,
			int stopper,		/* EOS or ')' */
			int type,			/* LACON (lookaround subRE) or PLAIN */
			struct state *left, /* leftmost state */
			struct state *right,	/* rightmost state */
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
		t = parseqatom(v, stopper, type, lp, right, t);
		NOERRN();
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
 *
 * Usually, the return value is just "top", but in some cases where we
 * have parsed the rest of the branch, we may deem "top" redundant and
 * free it, returning some child subre instead.
 */
static struct subre *
parseqatom(struct vars *v,
		   int stopper,			/* EOS or ')' */
		   int type,			/* LACON (lookaround subRE) or PLAIN */
		   struct state *lp,	/* left state to hang it on */
		   struct state *rp,	/* right state to hang it on */
		   struct subre *top)	/* subtree top */
{
	struct state *s;			/* temporaries for new states */
	struct state *s2;

#define  ARCV(t, val)	 newarc(v->nfa, t, val, lp, rp)
	int			m,
				n;
	struct subre *atom;			/* atom's subtree */
	struct subre *t;
	int			cap;			/* capturing parens? */
	int			latype;			/* lookaround constraint type */
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
			return top;
			break;
		case '$':
			ARCV('$', 1);
			if (v->cflags & REG_NLANCH)
				ARCV(AHEAD, v->nlcolor);
			NEXT();
			return top;
			break;
		case SBEGIN:
			ARCV('^', 1);		/* BOL */
			ARCV('^', 0);		/* or BOS */
			NEXT();
			return top;
			break;
		case SEND:
			ARCV('$', 1);		/* EOL */
			ARCV('$', 0);		/* or EOS */
			NEXT();
			return top;
			break;
		case '<':
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case '>':
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case WBDRY:
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case NWBDRY:
			wordchrs(v);
			s = newstate(v->nfa);
			NOERRN();
			word(v, BEHIND, lp, s);
			word(v, AHEAD, s, rp);
			s = newstate(v->nfa);
			NOERRN();
			nonword(v, BEHIND, lp, s);
			nonword(v, AHEAD, s, rp);
			NEXT();
			return top;
			break;
		case LACON:				/* lookaround constraint */
			latype = v->nextvalue;
			NEXT();
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERRN();
			t = parse(v, ')', LACON, s, s2);
			freesubre(v, t);	/* internal structure irrelevant */
			NOERRN();
			assert(SEE(')'));
			NEXT();
			processlacon(v, s, s2, latype, lp, rp);
			return top;
			break;
			/* then errors, to get them out of the way */
		case '*':
		case '+':
		case '?':
		case '{':
			ERR(REG_BADRPT);
			return top;
			break;
		default:
			ERR(REG_ASSERT);
			return top;
			break;
			/* then plain characters, and minor variants on that theme */
		case ')':				/* unbalanced paren */
			if ((v->cflags & REG_ADVANCED) != REG_EXTENDED)
			{
				ERR(REG_EPAREN);
				return top;
			}
			/* legal in EREs due to specification botch */
			NOTE(REG_UPBOTCH);
			/* fall through into case PLAIN */
			/* FALLTHROUGH */
		case PLAIN:
			onechr(v, v->nextvalue, lp, rp);
			okcolors(v->nfa, v->cm);
			NOERRN();
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
		case CCLASSS:
			charclass(v, (enum char_classes) v->nextvalue, lp, rp);
			okcolors(v->nfa, v->cm);
			NEXT();
			break;
		case CCLASSC:
			charclasscomplement(v, (enum char_classes) v->nextvalue, lp, rp);
			/* charclasscomplement() did okcolors() internally */
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
			}
			else
				atomtype = PLAIN;	/* something that's not '(' */
			NEXT();

			/*
			 * Make separate endpoint states to keep this sub-NFA distinct
			 * from what surrounds it.  We need to be sure that when we
			 * duplicate the sub-NFA for a backref, we get the right
			 * states/arcs and no others.  In particular, letting a backref
			 * duplicate the sub-NFA from lp to rp would be quite wrong,
			 * because we may add quantification superstructure around this
			 * atom below.  (Perhaps we could skip the extra states for
			 * non-capturing parens, but it seems not worth the trouble.)
			 */
			s = newstate(v->nfa);
			s2 = newstate(v->nfa);
			NOERRN();
			/* We may not need these arcs, but keep things connected for now */
			EMPTYARC(lp, s);
			EMPTYARC(s2, rp);
			NOERRN();
			atom = parse(v, ')', type, s, s2);
			assert(SEE(')') || ISERR());
			NEXT();
			NOERRN();
			if (cap)
			{
				if (atom->capno == 0)
				{
					/* normal case: just mark the atom as capturing */
					atom->flags |= CAP;
					atom->capno = subno;
				}
				else
				{
					/* generate no-op wrapper node to handle "((x))" */
					t = subre(v, '(', atom->flags | CAP, s, s2);
					NOERRN();
					t->capno = subno;
					t->child = atom;
					atom = t;
				}
				assert(v->subs[subno] == NULL);
				v->subs[subno] = atom;
			}
			/* postpone everything else pending possible {0} */
			break;
		case BACKREF:			/* the Feature From The Black Lagoon */
			INSIST(type != LACON, REG_ESUBREG);
			subno = v->nextvalue;
			assert(subno > 0);
			INSIST(subno < v->nsubs, REG_ESUBREG);
			NOERRN();
			INSIST(v->subs[subno] != NULL, REG_ESUBREG);
			NOERRN();
			atom = subre(v, 'b', BACKR, lp, rp);
			NOERRN();
			atom->backno = subno;
			v->subs[subno]->flags |= BRUSE;
			EMPTYARC(lp, rp);	/* temporarily, so there's something */
			NEXT();
			break;
	}

	/* ...and an atom may be followed by a quantifier */
	switch (v->nexttype)
	{
		case '*':
			m = 0;
			n = DUPINF;
			qprefer = (v->nextvalue) ? LONGER : SHORTER;
			NEXT();
			break;
		case '+':
			m = 1;
			n = DUPINF;
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
					n = DUPINF;
				if (m > n)
				{
					ERR(REG_BADBR);
					return top;
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
				return top;
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
		/*
		 * If we had capturing subexpression(s) within the atom, we don't want
		 * to destroy them, because it's legal (if useless) to back-ref them
		 * later.  Hence, just unlink the atom from lp/rp and then ignore it.
		 */
		if (atom != NULL && (atom->flags & CAP))
		{
			delsub(v->nfa, lp, atom->begin);
			delsub(v->nfa, atom->end, rp);
		}
		else
		{
			/* Otherwise, we can clean up any subre infrastructure we made */
			if (atom != NULL)
				freesubre(v, atom);
			delsub(v->nfa, lp, rp);
		}
		EMPTYARC(lp, rp);
		return top;
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
		return top;
	}

	/*
	 * hard part:  something messy
	 *
	 * That is, capturing parens, back reference, short/long clash, or an atom
	 * with substructure containing one of those.
	 */

	/* now we'll need a subre for the contents even if they're boring */
	if (atom == NULL)
	{
		atom = subre(v, '=', 0, lp, rp);
		NOERRN();
	}

	/*
	 * For what follows, we need the atom to have its own begin/end states
	 * that are distinct from lp/rp, so that we can wrap iteration structure
	 * around it.  The parenthesized-atom case above already made suitable
	 * states (and we don't want to modify a capturing subre, since it's
	 * already recorded in v->subs[]).  Otherwise, we need more states.
	 */
	if (atom->begin == lp || atom->end == rp)
	{
		s = newstate(v->nfa);
		s2 = newstate(v->nfa);
		NOERRN();
		moveouts(v->nfa, lp, s);
		moveins(v->nfa, rp, s2);
		atom->begin = s;
		atom->end = s2;
	}
	else
	{
		/* The atom's OK, but we must temporarily disconnect it from lp/rp */
		/* (this removes the EMPTY arcs we made above) */
		delsub(v->nfa, lp, atom->begin);
		delsub(v->nfa, atom->end, rp);
	}

	/*----------
	 * Prepare a general-purpose state skeleton.
	 *
	 * In the no-backrefs case, we want this:
	 *
	 * [lp] ---> [s] ---prefix---> ---atom---> ---rest---> [rp]
	 *
	 * where prefix is some repetitions of atom, and "rest" is the remainder
	 * of the branch.  In the general case we need:
	 *
	 * [lp] ---> [s] ---iterator---> [s2] ---rest---> [rp]
	 *
	 * where the iterator wraps around the atom.
	 *
	 * We make the s state here for both cases; s2 is made below if needed
	 *----------
	 */
	s = newstate(v->nfa);		/* set up starting state */
	NOERRN();
	EMPTYARC(lp, s);
	NOERRN();

	/* break remaining subRE into x{...} and what follows */
	t = subre(v, '.', COMBINE(qprefer, atom->flags), lp, rp);
	NOERRN();
	t->child = atom;
	atomp = &t->child;

	/*
	 * Here we should recurse to fill t->child->sibling ... but we must
	 * postpone that to the end.  One reason is that t->child may be replaced
	 * below, and we don't want to worry about its sibling link.
	 */

	/*
	 * Convert top node to a concatenation of the prefix (top->child, covering
	 * whatever we parsed previously) and remaining (t).  Note that the prefix
	 * could be empty, in which case this concatenation node is unnecessary.
	 * To keep things simple, we operate in a general way for now, and get rid
	 * of unnecessary subres below.
	 */
	assert(top->op == '=' && top->child == NULL);
	top->child = subre(v, '=', top->flags, top->begin, lp);
	NOERRN();
	top->op = '.';
	top->child->sibling = t;
	/* top->flags will get updated later */

	/* if it's a backref, now is the time to replicate the subNFA */
	if (atomtype == BACKREF)
	{
		assert(atom->begin->nouts == 1);	/* just the EMPTY */
		delsub(v->nfa, atom->begin, atom->end);
		assert(v->subs[subno] != NULL);

		/*
		 * And here's why the recursion got postponed: it must wait until the
		 * skeleton is filled in, because it may hit a backref that wants to
		 * copy the filled-in skeleton.
		 */
		dupnfa(v->nfa, v->subs[subno]->begin, v->subs[subno]->end,
			   atom->begin, atom->end);
		NOERRN();

		/* The backref node's NFA should not enforce any constraints */
		removeconstraints(v->nfa, atom->begin, atom->end);
		NOERRN();
	}

	/*
	 * It's quantifier time.  If the atom is just a backref, we'll let it deal
	 * with quantifiers internally.
	 */
	if (atomtype == BACKREF)
	{
		/* special case:  backrefs have internal quantifiers */
		EMPTYARC(s, atom->begin);	/* empty prefix */
		/* just stuff everything into atom */
		repeat(v, atom->begin, atom->end, m, n);
		atom->min = (short) m;
		atom->max = (short) n;
		atom->flags |= COMBINE(qprefer, atom->flags);
		/* rest of branch can be strung starting from atom->end */
		s2 = atom->end;
	}
	else if (m == 1 && n == 1 &&
			 (qprefer == 0 ||
			  (atom->flags & (LONGER | SHORTER | MIXED)) == 0 ||
			  qprefer == (atom->flags & (LONGER | SHORTER | MIXED))))
	{
		/* no/vacuous quantifier:  done */
		EMPTYARC(s, atom->begin);	/* empty prefix */
		/* rest of branch can be strung starting from atom->end */
		s2 = atom->end;
	}
	else if (!(atom->flags & (CAP | BACKR)))
	{
		/*
		 * If there's no captures nor backrefs in the atom being repeated, we
		 * don't really care where the submatches of the iteration are, so we
		 * don't need an iteration node.  Make a plain DFA node instead.
		 */
		EMPTYARC(s, atom->begin);	/* empty prefix */
		repeat(v, atom->begin, atom->end, m, n);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '=', f, atom->begin, atom->end);
		NOERRN();
		freesubre(v, atom);
		*atomp = t;
		/* rest of branch can be strung starting from t->end */
		s2 = t->end;
	}
	else if (m > 0 && !(atom->flags & BACKR))
	{
		/*
		 * If there's no backrefs involved, we can turn x{m,n} into
		 * x{m-1,n-1}x, with capturing parens in only the second x.  This is
		 * valid because we only care about capturing matches from the final
		 * iteration of the quantifier.  It's a win because we can implement
		 * the backref-free left side as a plain DFA node, since we don't
		 * really care where its submatches are.
		 */
		dupnfa(v->nfa, atom->begin, atom->end, s, atom->begin);
		assert(m >= 1 && m != DUPINF && n >= 1);
		repeat(v, s, atom->begin, m - 1, (n == DUPINF) ? n : n - 1);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '.', f, s, atom->end); /* prefix and atom */
		NOERRN();
		t->child = subre(v, '=', PREF(f), s, atom->begin);
		NOERRN();
		t->child->sibling = atom;
		*atomp = t;
		/* rest of branch can be strung starting from atom->end */
		s2 = atom->end;
	}
	else
	{
		/* general case: need an iteration node */
		s2 = newstate(v->nfa);
		NOERRN();
		moveouts(v->nfa, atom->end, s2);
		NOERRN();
		dupnfa(v->nfa, atom->begin, atom->end, s, s2);
		repeat(v, s, s2, m, n);
		f = COMBINE(qprefer, atom->flags);
		t = subre(v, '*', f, s, s2);
		NOERRN();
		t->min = (short) m;
		t->max = (short) n;
		t->child = atom;
		*atomp = t;
		/* rest of branch is to be strung from iteration's end state */
	}

	/* and finally, look after that postponed recursion */
	t = top->child->sibling;
	if (!(SEE('|') || SEE(stopper) || SEE(EOS)))
	{
		/* parse all the rest of the branch, and insert in t->child->sibling */
		t->child->sibling = parsebranch(v, stopper, type, s2, rp, 1);
		NOERRN();
		assert(SEE('|') || SEE(stopper) || SEE(EOS));

		/* here's the promised update of the flags */
		t->flags |= COMBINE(t->flags, t->child->sibling->flags);
		top->flags |= COMBINE(top->flags, t->flags);

		/* neither t nor top could be directly marked for capture as yet */
		assert(t->capno == 0);
		assert(top->capno == 0);

		/*
		 * At this point both top and t are concatenation (op == '.') subres,
		 * and we have top->child = prefix of branch, top->child->sibling = t,
		 * t->child = messy atom (with quantification superstructure if
		 * needed), t->child->sibling = rest of branch.
		 *
		 * If the messy atom was the first thing in the branch, then
		 * top->child is vacuous and we can get rid of one level of
		 * concatenation.
		 */
		assert(top->child->op == '=');
		if (top->child->begin == top->child->end)
		{
			assert(!MESSY(top->child->flags));
			freesubre(v, top->child);
			top->child = t->child;
			freesrnode(v, t);
		}

		/*
		 * Otherwise, it's possible that t->child is not messy in itself, but
		 * we considered it messy because its greediness conflicts with what
		 * preceded it.  Then it could be that the combination of t->child and
		 * the rest of the branch is also not messy, in which case we can get
		 * rid of the child concatenation by merging t->child and the rest of
		 * the branch into one plain DFA node.
		 */
		else if (t->child->op == '=' &&
				 t->child->sibling->op == '=' &&
				 !MESSY(UP(t->child->flags | t->child->sibling->flags)))
		{
			t->op = '=';
			t->flags = COMBINE(t->child->flags, t->child->sibling->flags);
			freesubreandsiblings(v, t->child);
			t->child = NULL;
		}
	}
	else
	{
		/*
		 * There's nothing left in the branch, so we don't need the second
		 * concatenation node 't'.  Just link s2 straight to rp.
		 */
		EMPTYARC(s2, rp);
		top->child->sibling = t->child;
		top->flags |= COMBINE(top->flags, top->child->sibling->flags);
		freesrnode(v, t);

		/*
		 * Again, it could be that top->child is vacuous (if the messy atom
		 * was in fact the only thing in the branch).  In that case we need no
		 * concatenation at all; just replace top with top->child->sibling.
		 */
		assert(top->child->op == '=');
		if (top->child->begin == top->child->end)
		{
			assert(!MESSY(top->child->flags));
			t = top->child->sibling;
			top->child->sibling = NULL;
			freesubre(v, top);
			top = t;
		}
	}

	return top;
}

/*
 * nonword - generate arcs for non-word-character ahead or behind
 */
static void
nonword(struct vars *v,
		int dir,				/* AHEAD or BEHIND */
		struct state *lp,
		struct state *rp)
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
word(struct vars *v,
	 int dir,					/* AHEAD or BEHIND */
	 struct state *lp,
	 struct state *rp)
{
	assert(dir == AHEAD || dir == BEHIND);
	cloneouts(v->nfa, v->wordchrs, lp, rp, dir);
	/* (no need for special attention to \n) */
}

/*
 * charclass - generate arcs for a character class
 *
 * This is used for both atoms (\w and sibling escapes) and for elements
 * of bracket expressions.  The caller is responsible for calling okcolors()
 * at the end of processing the atom or bracket.
 */
static void
charclass(struct vars *v,
		  enum char_classes cls,
		  struct state *lp,
		  struct state *rp)
{
	struct cvec *cv;

	/* obtain possibly-cached cvec for char class */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, cls, (v->cflags & REG_ICASE));
	NOERR();

	/* build the arcs; this may cause color splitting */
	subcolorcvec(v, cv, lp, rp);
}

/*
 * charclasscomplement - generate arcs for a complemented character class
 *
 * This is used for both atoms (\W and sibling escapes) and for elements
 * of bracket expressions.  In bracket expressions, it is the caller's
 * responsibility that there not be any open subcolors when this is called.
 */
static void
charclasscomplement(struct vars *v,
					enum char_classes cls,
					struct state *lp,
					struct state *rp)
{
	struct state *cstate;
	struct cvec *cv;

	/* make dummy state to hang temporary arcs on */
	cstate = newstate(v->nfa);
	NOERR();

	/* obtain possibly-cached cvec for char class */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, cls, (v->cflags & REG_ICASE));
	NOERR();

	/* build arcs for char class; this may cause color splitting */
	subcolorcvec(v, cv, cstate, cstate);
	NOERR();

	/* clean up any subcolors in the arc set */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* now build output arcs for the complement of the char class */
	colorcomplement(v->nfa, v->cm, PLAIN, cstate, lp, rp);
	NOERR();

	/* clean up dummy state */
	dropstate(v->nfa, cstate);
}

/*
 * scannum - scan a number
 */
static int						/* value, <= DUPMAX */
scannum(struct vars *v)
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
 * The sub-NFA strung from lp to rp is modified to represent m to n
 * repetitions of its initial contents.
 *
 * The duplication sequences used here are chosen carefully so that any
 * pointers starting out pointing into the subexpression end up pointing into
 * the last occurrence.  (Note that it may not be strung between the same
 * left and right end states, however!)  This used to be important for the
 * subRE tree, although the important bits are now handled by the in-line
 * code in parse(), and when this is called, it doesn't matter any more.
 */
static void
repeat(struct vars *v,
	   struct state *lp,
	   struct state *rp,
	   int m,
	   int n)
{
#define  SOME	 2
#define  INF	 3
#define  PAIR(x, y)  ((x)*4 + (y))
#define  REDUCE(x)	 ( ((x) == DUPINF) ? INF : (((x) > 1) ? SOME : (x)) )
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
 *
 * Also called from cbracket for complemented bracket expressions.
 */
static void
bracket(struct vars *v,
		struct state *lp,
		struct state *rp)
{
	/*
	 * We can't process complemented char classes (e.g. \W) immediately while
	 * scanning the bracket expression, else color bookkeeping gets confused.
	 * Instead, remember whether we saw any in have_cclassc[], and process
	 * them at the end.
	 */
	bool		have_cclassc[NUM_CCLASSES];
	bool		any_cclassc;
	int			i;

	memset(have_cclassc, false, sizeof(have_cclassc));

	assert(SEE('['));
	NEXT();
	while (!SEE(']') && !SEE(EOS))
		brackpart(v, lp, rp, have_cclassc);
	assert(SEE(']') || ISERR());

	/* close up open subcolors from the positive bracket elements */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* now handle any complemented elements */
	any_cclassc = false;
	for (i = 0; i < NUM_CCLASSES; i++)
	{
		if (have_cclassc[i])
		{
			charclasscomplement(v, (enum char_classes) i, lp, rp);
			NOERR();
			any_cclassc = true;
		}
	}

	/*
	 * If we had any complemented elements, see if we can optimize the bracket
	 * into a rainbow.  Since a complemented element is the only way a WHITE
	 * arc could get into the result, there's no point in checking otherwise.
	 */
	if (any_cclassc)
		optimizebracket(v, lp, rp);
}

/*
 * cbracket - handle complemented bracket expression
 *
 * We do it by calling bracket() with dummy endpoints, and then complementing
 * the result.  The alternative would be to invoke rainbow(), and then delete
 * arcs as the b.e. is seen... but that gets messy, and is really quite
 * infeasible now that rainbow() just puts out one RAINBOW arc.
 */
static void
cbracket(struct vars *v,
		 struct state *lp,
		 struct state *rp)
{
	struct state *left = newstate(v->nfa);
	struct state *right = newstate(v->nfa);

	NOERR();
	bracket(v, left, right);

	/* in NLSTOP mode, ensure newline is not part of the result set */
	if (v->cflags & REG_NLSTOP)
		newarc(v->nfa, PLAIN, v->nlcolor, left, right);
	NOERR();

	assert(lp->nouts == 0);		/* all outarcs will be ours */

	/*
	 * Easy part of complementing, and all there is to do since the MCCE code
	 * was removed.  Note that the result of colorcomplement() cannot be a
	 * rainbow, since we don't allow empty brackets; so there's no point in
	 * calling optimizebracket() again.
	 */
	colorcomplement(v->nfa, v->cm, PLAIN, left, lp, rp);
	NOERR();
	dropstate(v->nfa, left);
	assert(right->nins == 0);
	freestate(v->nfa, right);
}

/*
 * brackpart - handle one item (or range) within a bracket expression
 */
static void
brackpart(struct vars *v,
		  struct state *lp,
		  struct state *rp,
		  bool *have_cclassc)
{
	chr			startc;
	chr			endc;
	struct cvec *cv;
	enum char_classes cls;
	const chr  *startp;
	const chr  *endp;

	/* parse something, get rid of special cases, take shortcuts */
	switch (v->nexttype)
	{
		case RANGE:				/* a-b-c or other botch */
			ERR(REG_ERANGE);
			return;
			break;
		case PLAIN:
			startc = v->nextvalue;
			NEXT();
			/* shortcut for ordinary chr (not range) */
			if (!SEE(RANGE))
			{
				onechr(v, startc, lp, rp);
				return;
			}
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
			subcolorcvec(v, cv, lp, rp);
			return;
			break;
		case CCLASS:
			startp = v->now;
			endp = scanplain(v);
			INSIST(startp < endp, REG_ECTYPE);
			NOERR();
			cls = lookupcclass(v, startp, endp);
			NOERR();
			charclass(v, cls, lp, rp);
			return;
			break;
		case CCLASSS:
			charclass(v, (enum char_classes) v->nextvalue, lp, rp);
			NEXT();
			return;
			break;
		case CCLASSC:
			/* we cannot call charclasscomplement() immediately */
			have_cclassc[v->nextvalue] = true;
			NEXT();
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
				endc = v->nextvalue;
				NEXT();
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
	subcolorcvec(v, cv, lp, rp);
}

/*
 * scanplain - scan PLAIN contents of [. etc.
 *
 * Certain bits of trickery in regc_lex.c know that this code does not try
 * to look past the final bracket of the [. etc.
 */
static const chr *				/* just after end of sequence */
scanplain(struct vars *v)
{
	const chr  *endp;

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
 * onechr - fill in arcs for a plain character, and possible case complements
 * This is mostly a shortcut for efficient handling of the common case.
 */
static void
onechr(struct vars *v,
	   chr c,
	   struct state *lp,
	   struct state *rp)
{
	if (!(v->cflags & REG_ICASE))
	{
		color		lastsubcolor = COLORLESS;

		subcoloronechr(v, c, lp, rp, &lastsubcolor);
		return;
	}

	/* rats, need general case anyway... */
	subcolorcvec(v, allcases(v, c), lp, rp);
}

/*
 * optimizebracket - see if bracket expression can be converted to RAINBOW
 *
 * Cases such as "[\s\S]" can produce a set of arcs of all colors, which we
 * can replace by a single RAINBOW arc for efficiency.  (This might seem
 * like a silly way to write ".", but it's seemingly a common locution in
 * some other flavors of regex, so take the trouble to support it well.)
 */
static void
optimizebracket(struct vars *v,
				struct state *lp,
				struct state *rp)
{
	struct colordesc *cd;
	struct colordesc *end = CDEND(v->cm);
	struct arc *a;
	bool		israinbow;

	/*
	 * Scan lp's out-arcs and transiently mark the mentioned colors.  We
	 * expect that all of lp's out-arcs are plain, non-RAINBOW arcs to rp.
	 * (Note: there shouldn't be any pseudocolors yet, but check anyway.)
	 */
	for (a = lp->outs; a != NULL; a = a->outchain)
	{
		assert(a->type == PLAIN);
		assert(a->co >= 0);		/* i.e. not RAINBOW */
		assert(a->to == rp);
		cd = &v->cm->cd[a->co];
		assert(!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO));
		cd->flags |= COLMARK;
	}

	/* Scan colors, clear transient marks, check for unmarked live colors */
	israinbow = true;
	for (cd = v->cm->cd; cd < end; cd++)
	{
		if (cd->flags & COLMARK)
			cd->flags &= ~COLMARK;
		else if (!UNUSEDCOLOR(cd) && !(cd->flags & PSEUDO))
			israinbow = false;
	}

	/* Can't do anything if not all colors have arcs */
	if (!israinbow)
		return;

	/* OK, drop existing arcs and replace with a rainbow */
	while ((a = lp->outs) != NULL)
		freearc(v->nfa, a);
	newarc(v->nfa, PLAIN, RAINBOW, lp, rp);
}

/*
 * wordchrs - set up word-chr list for word-boundary stuff, if needed
 *
 * The list is kept as a bunch of circular arcs on an otherwise-unused state.
 *
 * Note that this must not be called while we have any open subcolors,
 * else construction of the list would confuse color bookkeeping.
 * Hence, we can't currently apply a similar optimization in
 * charclass[complement](), as those need to be usable within bracket
 * expressions.
 */
static void
wordchrs(struct vars *v)
{
	struct state *cstate;
	struct cvec *cv;

	if (v->wordchrs != NULL)
		return;					/* done already */

	/* make dummy state to hang the cache arcs on */
	cstate = newstate(v->nfa);
	NOERR();

	/* obtain possibly-cached cvec for \w characters */
	NOTE(REG_ULOCALE);
	cv = cclasscvec(v, CC_WORD, (v->cflags & REG_ICASE));
	NOERR();

	/* build the arcs; this may cause color splitting */
	subcolorcvec(v, cv, cstate, cstate);
	NOERR();

	/* close new open subcolors to ensure the cache entry is self-contained */
	okcolors(v->nfa, v->cm);
	NOERR();

	/* success! save the cache pointer */
	v->wordchrs = cstate;
}

/*
 * processlacon - generate the NFA representation of a LACON
 *
 * In the general case this is just newlacon() + newarc(), but some cases
 * can be optimized.
 */
static void
processlacon(struct vars *v,
			 struct state *begin,	/* start of parsed LACON sub-re */
			 struct state *end, /* end of parsed LACON sub-re */
			 int latype,
			 struct state *lp,	/* left state to hang it on */
			 struct state *rp)	/* right state to hang it on */
{
	struct state *s1;
	int			n;

	/*
	 * Check for lookaround RE consisting of a single plain color arc (or set
	 * of arcs); this would typically be a simple chr or a bracket expression.
	 */
	s1 = single_color_transition(begin, end);
	switch (latype)
	{
		case LATYPE_AHEAD_POS:
			/* If lookahead RE is just colorset C, convert to AHEAD(C) */
			if (s1 != NULL)
			{
				cloneouts(v->nfa, s1, lp, rp, AHEAD);
				return;
			}
			break;
		case LATYPE_AHEAD_NEG:
			/* If lookahead RE is just colorset C, convert to AHEAD(^C)|$ */
			if (s1 != NULL)
			{
				colorcomplement(v->nfa, v->cm, AHEAD, s1, lp, rp);
				newarc(v->nfa, '$', 1, lp, rp);
				newarc(v->nfa, '$', 0, lp, rp);
				return;
			}
			break;
		case LATYPE_BEHIND_POS:
			/* If lookbehind RE is just colorset C, convert to BEHIND(C) */
			if (s1 != NULL)
			{
				cloneouts(v->nfa, s1, lp, rp, BEHIND);
				return;
			}
			break;
		case LATYPE_BEHIND_NEG:
			/* If lookbehind RE is just colorset C, convert to BEHIND(^C)|^ */
			if (s1 != NULL)
			{
				colorcomplement(v->nfa, v->cm, BEHIND, s1, lp, rp);
				newarc(v->nfa, '^', 1, lp, rp);
				newarc(v->nfa, '^', 0, lp, rp);
				return;
			}
			break;
		default:
			assert(NOTREACHED);
	}

	/* General case: we need a LACON subre and arc */
	n = newlacon(v, begin, end, latype);
	newarc(v->nfa, LACON, n, lp, rp);
}

/*
 * subre - allocate a subre
 */
static struct subre *
subre(struct vars *v,
	  int op,
	  int flags,
	  struct state *begin,
	  struct state *end)
{
	struct subre *ret = v->treefree;

	/*
	 * Checking for stack overflow here is sufficient to protect parse() and
	 * its recursive subroutines.
	 */
	if (STACK_TOO_DEEP(v->re))
	{
		ERR(REG_ETOOBIG);
		return NULL;
	}

	if (ret != NULL)
		v->treefree = ret->child;
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

	assert(strchr("=b|.*(", op) != NULL);

	ret->op = op;
	ret->flags = flags;
	ret->latype = (char) -1;
	ret->id = 0;				/* will be assigned later */
	ret->capno = 0;
	ret->backno = 0;
	ret->min = ret->max = 1;
	ret->child = NULL;
	ret->sibling = NULL;
	ret->begin = begin;
	ret->end = end;
	ZAPCNFA(ret->cnfa);

	return ret;
}

/*
 * freesubre - free a subRE subtree
 *
 * This frees child node(s) of the given subRE too,
 * but not its siblings.
 */
static void
freesubre(struct vars *v,		/* might be NULL */
		  struct subre *sr)
{
	if (sr == NULL)
		return;

	if (sr->child != NULL)
		freesubreandsiblings(v, sr->child);

	freesrnode(v, sr);
}

/*
 * freesubreandsiblings - free a subRE subtree
 *
 * This frees child node(s) of the given subRE too,
 * as well as any following siblings.
 */
static void
freesubreandsiblings(struct vars *v,	/* might be NULL */
					 struct subre *sr)
{
	while (sr != NULL)
	{
		struct subre *next = sr->sibling;

		freesubre(v, sr);
		sr = next;
	}
}

/*
 * freesrnode - free one node in a subRE subtree
 */
static void
freesrnode(struct vars *v,		/* might be NULL */
		   struct subre *sr)
{
	if (sr == NULL)
		return;

	if (!NULLCNFA(sr->cnfa))
		freecnfa(&sr->cnfa);
	sr->flags = 0;				/* in particular, not INUSE */
	sr->child = sr->sibling = NULL;
	sr->begin = sr->end = NULL;

	if (v != NULL && v->treechain != NULL)
	{
		/* we're still parsing, maybe we can reuse the subre */
		sr->child = v->treefree;
		v->treefree = sr;
	}
	else
		FREE(sr);
}

/*
 * removecaptures - remove unnecessary capture subREs
 *
 * If the caller said that it doesn't care about subexpression match data,
 * we may delete the "capture" markers on subREs that are not referenced
 * by any backrefs, and then simplify anything that's become non-messy.
 * Call this only if REG_NOSUB flag is set.
 */
static void
removecaptures(struct vars *v,
			   struct subre *t)
{
	struct subre *t2;

	assert(t != NULL);

	/*
	 * If this isn't itself a backref target, clear capno and tentatively
	 * clear CAP flag.
	 */
	if (!(t->flags & BRUSE))
	{
		t->capno = 0;
		t->flags &= ~CAP;
	}

	/* Now recurse to children */
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
	{
		removecaptures(v, t2);
		/* Propagate child CAP flag back up, if it's still set */
		if (t2->flags & CAP)
			t->flags |= CAP;
	}

	/*
	 * If t now contains neither captures nor backrefs, there's no longer any
	 * need to care where its sub-match boundaries are, so we can reduce it to
	 * a simple DFA node.  (Note in particular that MIXED child greediness is
	 * not a hindrance here, so we don't use the MESSY() macro.)
	 */
	if ((t->flags & (CAP | BACKR)) == 0)
	{
		if (t->child)
			freesubreandsiblings(v, t->child);
		t->child = NULL;
		t->op = '=';
		t->flags &= ~MIXED;
	}
}

/*
 * numst - number tree nodes (assigning "id" indexes)
 */
static int						/* next number */
numst(struct subre *t,
	  int start)				/* starting point for subtree numbers */
{
	int			i;
	struct subre *t2;

	assert(t != NULL);

	i = start;
	t->id = i++;
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		i = numst(t2, i);
	return i;
}

/*
 * markst - mark tree nodes as INUSE
 *
 * Note: this is a great deal more subtle than it looks.  During initial
 * parsing of a regex, all subres are linked into the treechain list;
 * discarded ones are also linked into the treefree list for possible reuse.
 * After we are done creating all subres required for a regex, we run markst()
 * then cleanst(), which results in discarding all subres not reachable from
 * v->tree.  We then clear v->treechain, indicating that subres must be found
 * by descending from v->tree.  This changes the behavior of freesubre(): it
 * will henceforth FREE() unwanted subres rather than sticking them into the
 * treefree list.  (Doing that any earlier would result in dangling links in
 * the treechain list.)  This all means that freev() will clean up correctly
 * if invoked before or after markst()+cleanst(); but it would not work if
 * called partway through this state conversion, so we mustn't error out
 * in or between these two functions.
 */
static void
markst(struct subre *t)
{
	struct subre *t2;

	assert(t != NULL);

	t->flags |= INUSE;
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		markst(t2);
}

/*
 * cleanst - free any tree nodes not marked INUSE
 */
static void
cleanst(struct vars *v)
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
nfatree(struct vars *v,
		struct subre *t,
		FILE *f)				/* for debug output */
{
	struct subre *t2;

	assert(t != NULL && t->begin != NULL);

	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		(DISCARD) nfatree(v, t2, f);

	return nfanode(v, t, 0, f);
}

/*
 * nfanode - do one NFA for nfatree or lacons
 *
 * If converttosearch is true, apply makesearch() to the NFA.
 */
static long						/* optimize results */
nfanode(struct vars *v,
		struct subre *t,
		int converttosearch,
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
	nfa->flags = v->nfa->flags;
	if (!ISERR())
		specialcolors(nfa);
	if (!ISERR())
		ret = optimize(nfa, f);
	if (converttosearch && !ISERR())
		makesearch(v, nfa);
	if (!ISERR())
		compact(nfa, &t->cnfa);

	freenfa(nfa);
	return ret;
}

/*
 * newlacon - allocate a lookaround-constraint subRE
 */
static int						/* lacon number */
newlacon(struct vars *v,
		 struct state *begin,
		 struct state *end,
		 int latype)
{
	int			n;
	struct subre *newlacons;
	struct subre *sub;

	if (v->nlacons == 0)
	{
		n = 1;					/* skip 0th */
		newlacons = (struct subre *) MALLOC(2 * sizeof(struct subre));
	}
	else
	{
		n = v->nlacons;
		newlacons = (struct subre *) REALLOC(v->lacons,
											 (n + 1) * sizeof(struct subre));
	}
	if (newlacons == NULL)
	{
		ERR(REG_ESPACE);
		return 0;
	}
	v->lacons = newlacons;
	v->nlacons = n + 1;
	sub = &v->lacons[n];
	sub->begin = begin;
	sub->end = end;
	sub->latype = latype;
	ZAPCNFA(sub->cnfa);
	return n;
}

/*
 * freelacons - free lookaround-constraint subRE vector
 */
static void
freelacons(struct subre *subs,
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
	if (g != NULL)
	{
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
}

/*
 * rstacktoodeep - check for stack getting dangerously deep
 *
 * Return nonzero to fail the operation with error code REG_ETOOBIG,
 * zero to keep going
 *
 * The current implementation is Postgres-specific.  If we ever get around
 * to splitting the regex code out as a standalone library, there will need
 * to be some API to let applications define a callback function for this.
 */
static int
rstacktoodeep(void)
{
	return stack_is_too_deep();
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
			(int) re->re_nsub, re->re_info, re->re_csize, g->ntree);

	dumpcolors(&g->cmap, f);
	if (!NULLCNFA(g->search))
	{
		fprintf(f, "\nsearch:\n");
		dumpcnfa(&g->search, f);
	}
	for (i = 1; i < g->nlacons; i++)
	{
		struct subre *lasub = &g->lacons[i];
		const char *latype;

		switch (lasub->latype)
		{
			case LATYPE_AHEAD_POS:
				latype = "positive lookahead";
				break;
			case LATYPE_AHEAD_NEG:
				latype = "negative lookahead";
				break;
			case LATYPE_BEHIND_POS:
				latype = "positive lookbehind";
				break;
			case LATYPE_BEHIND_NEG:
				latype = "negative lookbehind";
				break;
			default:
				latype = "???";
				break;
		}
		fprintf(f, "\nla%d (%s):\n", i, latype);
		dumpcnfa(&lasub->cnfa, f);
	}
	fprintf(f, "\n");
	dumpst(g->tree, f, 0);
}

/*
 * dumpst - dump a subRE tree
 */
static void
dumpst(struct subre *t,
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
stdump(struct subre *t,
	   FILE *f,
	   int nfapresent)			/* is the original NFA still around? */
{
	char		idbuf[50];
	struct subre *t2;

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
	if (t->flags & BRUSE)
		fprintf(f, " isreferenced");
	if (!(t->flags & INUSE))
		fprintf(f, " UNUSED");
	if (t->latype != (char) -1)
		fprintf(f, " latype(%d)", t->latype);
	if (t->capno != 0)
		fprintf(f, " capture(%d)", t->capno);
	if (t->backno != 0)
		fprintf(f, " backref(%d)", t->backno);
	if (t->min != 1 || t->max != 1)
	{
		fprintf(f, " {%d,", t->min);
		if (t->max != DUPINF)
			fprintf(f, "%d", t->max);
		fprintf(f, "}");
	}
	if (nfapresent)
		fprintf(f, " %ld-%ld", (long) t->begin->no, (long) t->end->no);
	if (t->child != NULL)
		fprintf(f, " C:%s", stid(t->child, idbuf, sizeof(idbuf)));
	/* printing second child isn't necessary, but it is often helpful */
	if (t->child != NULL && t->child->sibling != NULL)
		fprintf(f, " C2:%s", stid(t->child->sibling, idbuf, sizeof(idbuf)));
	if (t->sibling != NULL)
		fprintf(f, " S:%s", stid(t->sibling, idbuf, sizeof(idbuf)));
	if (!NULLCNFA(t->cnfa))
	{
		fprintf(f, "\n");
		dumpcnfa(&t->cnfa, f);
	}
	fprintf(f, "\n");
	for (t2 = t->child; t2 != NULL; t2 = t2->sibling)
		stdump(t2, f, nfapresent);
}

/*
 * stid - identify a subtree node for dumping
 */
static const char *				/* points to buf or constant string */
stid(struct subre *t,
	 char *buf,
	 size_t bufsize)
{
	/* big enough for hex int or decimal t->id? */
	if (bufsize < sizeof(void *) * 2 + 3 || bufsize < sizeof(t->id) * 3 + 1)
		return "unable";
	if (t->id != 0)
		sprintf(buf, "%d", t->id);
	else
		sprintf(buf, "%p", t);
	return buf;
}
#endif							/* REG_DEBUG */


#include "regc_lex.c"
#include "regc_color.c"
#include "regc_nfa.c"
#include "regc_cvec.c"
#include "regc_pg_locale.c"
#include "regc_locale.c"
