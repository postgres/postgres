/*
 * Internal interface definitions, etc., for the reg package
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
 * src/include/regex/regguts.h
 */



/*
 * Environmental customization.  It should not (I hope) be necessary to
 * alter the file you are now reading -- regcustom.h should handle it all,
 * given care here and elsewhere.
 */
#include "regcustom.h"



/*
 * Things that regcustom.h might override.
 */

/* assertions */
#ifndef assert
#ifndef REG_DEBUG
#define  NDEBUG					/* no assertions */
#endif
#include <assert.h>
#endif

/* voids */
#ifndef DISCARD
#define DISCARD void			/* for throwing values away */
#endif
#ifndef VS
#define VS(x)	((void *)(x))	/* cast something to generic ptr */
#endif

/* function-pointer declarator */
#ifndef FUNCPTR
#define FUNCPTR(name, args) (*(name)) args
#endif

/* memory allocation */
#ifndef MALLOC
#define MALLOC(n)	malloc(n)
#endif
#ifndef REALLOC
#define REALLOC(p, n)	realloc(VS(p), n)
#endif
#ifndef FREE
#define FREE(p)		free(VS(p))
#endif

/* want size of a char in bits, and max value in bounded quantifiers */
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX	255 /* normally from <limits.h> */
#endif



/*
 * misc
 */

#define NOTREACHED	0

#define DUPMAX	_POSIX2_RE_DUP_MAX
#define DUPINF	(DUPMAX+1)

#define REMAGIC 0xfed7			/* magic number for main struct */

/* Type codes for lookaround constraints */
#define LATYPE_AHEAD_POS	03	/* positive lookahead */
#define LATYPE_AHEAD_NEG	02	/* negative lookahead */
#define LATYPE_BEHIND_POS	01	/* positive lookbehind */
#define LATYPE_BEHIND_NEG	00	/* negative lookbehind */
#define LATYPE_IS_POS(la)	((la) & 01)
#define LATYPE_IS_AHEAD(la) ((la) & 02)


/*
 * debugging facilities
 */
#ifdef REG_DEBUG
/* FDEBUG does finite-state tracing */
#define FDEBUG(arglist) { if (v->eflags&REG_FTRACE) printf arglist; }
/* MDEBUG does higher-level tracing */
#define MDEBUG(arglist) { if (v->eflags&REG_MTRACE) printf arglist; }
#else
#define FDEBUG(arglist) {}
#define MDEBUG(arglist) {}
#endif



/*
 * bitmap manipulation
 */
#define UBITS	(CHAR_BIT * sizeof(unsigned))
#define BSET(uv, sn)	((uv)[(sn)/UBITS] |= (unsigned)1 << ((sn)%UBITS))
#define ISBSET(uv, sn)	((uv)[(sn)/UBITS] & ((unsigned)1 << ((sn)%UBITS)))


/*
 * As soon as possible, we map chrs into equivalence classes -- "colors" --
 * which are of much more manageable number.
 */
typedef short color;			/* colors of characters */

#define MAX_COLOR	32767		/* max color (must fit in 'color' datatype) */
#define COLORLESS	(-1)		/* impossible color */
#define WHITE		0			/* default color, parent of all others */
/* Note: various places in the code know that WHITE is zero */


/*
 * Per-color data structure for the compile-time color machinery
 *
 * If "sub" is not NOSUB then it is the number of the color's current
 * subcolor, i.e. we are in process of dividing this color (character
 * equivalence class) into two colors.  See src/backend/regex/README for
 * discussion of subcolors.
 *
 * Currently-unused colors have the FREECOL bit set and are linked into a
 * freelist using their "sub" fields, but only if their color numbers are
 * less than colormap.max.  Any array entries beyond "max" are just garbage.
 */
struct colordesc
{
	int			nschrs;			/* number of simple chars of this color */
	int			nuchrs;			/* number of upper map entries of this color */
	color		sub;			/* open subcolor, if any; or free-chain ptr */
#define  NOSUB	 COLORLESS		/* value of "sub" when no open subcolor */
	struct arc *arcs;			/* chain of all arcs of this color */
	chr			firstchr;		/* simple char first assigned to this color */
	int			flags;			/* bit values defined next */
#define  FREECOL 01				/* currently free */
#define  PSEUDO  02				/* pseudocolor, no real chars */
#define  UNUSEDCOLOR(cd) ((cd)->flags & FREECOL)
};

/*
 * The color map itself
 *
 * This struct holds both data used only at compile time, and the chr to
 * color mapping information, used at both compile and run time.  The latter
 * is the bulk of the space, so it's not really worth separating out the
 * compile-only portion.
 *
 * Ideally, the mapping data would just be an array of colors indexed by
 * chr codes; but for large character sets that's impractical.  Fortunately,
 * common characters have smaller codes, so we can use a simple array for chr
 * codes up to MAX_SIMPLE_CHR, and do something more complex for codes above
 * that, without much loss of performance.  The "something more complex" is a
 * 2-D array of color entries, where row indexes correspond to individual chrs
 * or chr ranges that have been mentioned in the regex (with row zero
 * representing all other chrs), and column indexes correspond to different
 * sets of locale-dependent character classes such as "isalpha".  The
 * classbits[k] entry is zero if we do not care about the k'th character class
 * in this regex, and otherwise it is the bit to be OR'd into the column index
 * if the character in question is a member of that class.  We find the color
 * of a high-valued chr by identifying which colormaprange it is in to get
 * the row index (use row zero if it's in none of them), identifying which of
 * the interesting cclasses it's in to get the column index, and then indexing
 * into the 2-D hicolormap array.
 *
 * The colormapranges are required to be nonempty, nonoverlapping, and to
 * appear in increasing chr-value order.
 */

#define NUM_CCLASSES 13			/* must match data in regc_locale.c */

typedef struct colormaprange
{
	chr			cmin;			/* range represents cmin..cmax inclusive */
	chr			cmax;
	int			rownum;			/* row index in hicolormap array (>= 1) */
} colormaprange;

struct colormap
{
	int			magic;
#define  CMMAGIC 0x876
	struct vars *v;				/* for compile error reporting */
	size_t		ncds;			/* allocated length of colordescs array */
	size_t		max;			/* highest color number currently in use */
	color		free;			/* beginning of free chain (if non-0) */
	struct colordesc *cd;		/* pointer to array of colordescs */
#define  CDEND(cm)	 (&(cm)->cd[(cm)->max + 1])

	/* mapping data for chrs <= MAX_SIMPLE_CHR: */
	color	   *locolormap;		/* simple array indexed by chr code */

	/* mapping data for chrs > MAX_SIMPLE_CHR: */
	int			classbits[NUM_CCLASSES];	/* see comment above */
	int			numcmranges;	/* number of colormapranges */
	colormaprange *cmranges;	/* ranges of high chrs */
	color	   *hicolormap;		/* 2-D array of color entries */
	int			maxarrayrows;	/* number of array rows allocated */
	int			hiarrayrows;	/* number of array rows in use */
	int			hiarraycols;	/* number of array columns (2^N) */

	/* If we need up to NINLINECDS, we store them here to save a malloc */
#define  NINLINECDS  ((size_t) 10)
	struct colordesc cdspace[NINLINECDS];
};

/* fetch color for chr; beware of multiple evaluation of c argument */
#define GETCOLOR(cm, c) \
	((c) <= MAX_SIMPLE_CHR ? (cm)->locolormap[(c) - CHR_MIN] : pg_reg_getcolor(cm, c))


/*
 * Interface definitions for locale-interface functions in regc_locale.c.
 */

/*
 * Representation of a set of characters.  chrs[] represents individual
 * code points, ranges[] represents ranges in the form min..max inclusive.
 *
 * If the cvec represents a locale-specific character class, eg [[:alpha:]],
 * then the chrs[] and ranges[] arrays contain only members of that class
 * up to MAX_SIMPLE_CHR (inclusive).  cclasscode is set to regc_locale.c's
 * code for the class, rather than being -1 as it is in an ordinary cvec.
 *
 * Note that in cvecs gotten from newcvec() and intended to be freed by
 * freecvec(), both arrays of chrs are after the end of the struct, not
 * separately malloc'd; so chrspace and rangespace are effectively immutable.
 */
struct cvec
{
	int			nchrs;			/* number of chrs */
	int			chrspace;		/* number of chrs allocated in chrs[] */
	chr		   *chrs;			/* pointer to vector of chrs */
	int			nranges;		/* number of ranges (chr pairs) */
	int			rangespace;		/* number of ranges allocated in ranges[] */
	chr		   *ranges;			/* pointer to vector of chr pairs */
	int			cclasscode;		/* value of "enum classes", or -1 */
};


/*
 * definitions for NFA internal representation
 *
 * Having a "from" pointer within each arc may seem redundant, but it
 * saves a lot of hassle.
 */
struct state;

struct arc
{
	int			type;			/* 0 if free, else an NFA arc type code */
	color		co;
	struct state *from;			/* where it's from (and contained within) */
	struct state *to;			/* where it's to */
	struct arc *outchain;		/* link in *from's outs chain or free chain */
	struct arc *outchainRev;	/* back-link in *from's outs chain */
#define  freechain	outchain	/* we do not maintain "freechainRev" */
	struct arc *inchain;		/* link in *to's ins chain */
	struct arc *inchainRev;		/* back-link in *to's ins chain */
	struct arc *colorchain;		/* link in color's arc chain */
	struct arc *colorchainRev;	/* back-link in color's arc chain */
};

struct arcbatch
{								/* for bulk allocation of arcs */
	struct arcbatch *next;
#define  ABSIZE  10
	struct arc	a[ABSIZE];
};

struct state
{
	int			no;
#define  FREESTATE	 (-1)
	char		flag;			/* marks special states */
	int			nins;			/* number of inarcs */
	struct arc *ins;			/* chain of inarcs */
	int			nouts;			/* number of outarcs */
	struct arc *outs;			/* chain of outarcs */
	struct arc *free;			/* chain of free arcs */
	struct state *tmp;			/* temporary for traversal algorithms */
	struct state *next;			/* chain for traversing all */
	struct state *prev;			/* back chain */
	struct arcbatch oas;		/* first arcbatch, avoid malloc in easy case */
	int			noas;			/* number of arcs used in first arcbatch */
};

struct nfa
{
	struct state *pre;			/* pre-initial state */
	struct state *init;			/* initial state */
	struct state *final;		/* final state */
	struct state *post;			/* post-final state */
	int			nstates;		/* for numbering states */
	struct state *states;		/* state-chain header */
	struct state *slast;		/* tail of the chain */
	struct state *free;			/* free list */
	struct colormap *cm;		/* the color map */
	color		bos[2];			/* colors, if any, assigned to BOS and BOL */
	color		eos[2];			/* colors, if any, assigned to EOS and EOL */
	struct vars *v;				/* simplifies compile error reporting */
	struct nfa *parent;			/* parent NFA, if any */
};



/*
 * definitions for compacted NFA
 *
 * The main space savings in a compacted NFA is from making the arcs as small
 * as possible.  We store only the transition color and next-state number for
 * each arc.  The list of out arcs for each state is an array beginning at
 * cnfa.states[statenumber], and terminated by a dummy carc struct with
 * co == COLORLESS.
 *
 * The non-dummy carc structs are of two types: plain arcs and LACON arcs.
 * Plain arcs just store the transition color number as "co".  LACON arcs
 * store the lookaround constraint number plus cnfa.ncolors as "co".  LACON
 * arcs can be distinguished from plain by testing for co >= cnfa.ncolors.
 */
struct carc
{
	color		co;				/* COLORLESS is list terminator */
	int			to;				/* next-state number */
};

struct cnfa
{
	int			nstates;		/* number of states */
	int			ncolors;		/* number of colors (max color in use + 1) */
	int			flags;
#define  HASLACONS	01			/* uses lookaround constraints */
	int			pre;			/* setup state number */
	int			post;			/* teardown state number */
	color		bos[2];			/* colors, if any, assigned to BOS and BOL */
	color		eos[2];			/* colors, if any, assigned to EOS and EOL */
	char	   *stflags;		/* vector of per-state flags bytes */
#define  CNFA_NOPROGRESS	01	/* flag bit for a no-progress state */
	struct carc **states;		/* vector of pointers to outarc lists */
	/* states[n] are pointers into a single malloc'd array of arcs */
	struct carc *arcs;			/* the area for the lists */
};

#define ZAPCNFA(cnfa)	((cnfa).nstates = 0)
#define NULLCNFA(cnfa)	((cnfa).nstates == 0)

/*
 * This symbol limits the transient heap space used by the regex compiler,
 * and thereby also the maximum complexity of NFAs that we'll deal with.
 * Currently we only count NFA states and arcs against this; the other
 * transient data is generally not large enough to notice compared to those.
 * Note that we do not charge anything for the final output data structures
 * (the compacted NFA and the colormap).
 */
#ifndef REG_MAX_COMPILE_SPACE
#define REG_MAX_COMPILE_SPACE  \
	(100000 * sizeof(struct state) + 100000 * sizeof(struct arcbatch))
#endif

/*
 * subexpression tree
 *
 * "op" is one of:
 *		'='  plain regex without interesting substructure (implemented as DFA)
 *		'b'  back-reference (has no substructure either)
 *		'('  capture node: captures the match of its single child
 *		'.'  concatenation: matches a match for left, then a match for right
 *		'|'  alternation: matches a match for left or a match for right
 *		'*'  iteration: matches some number of matches of its single child
 *
 * Note: the right child of an alternation must be another alternation or
 * NULL; hence, an N-way branch requires N alternation nodes, not N-1 as you
 * might expect.  This could stand to be changed.  Actually I'd rather see
 * a single alternation node with N children, but that will take revising
 * the representation of struct subre.
 *
 * Note: when a backref is directly quantified, we stick the min/max counts
 * into the backref rather than plastering an iteration node on top.  This is
 * for efficiency: there is no need to search for possible division points.
 */
struct subre
{
	char		op;				/* see type codes above */
	char		flags;
#define  LONGER  01				/* prefers longer match */
#define  SHORTER 02				/* prefers shorter match */
#define  MIXED	 04				/* mixed preference below */
#define  CAP	 010			/* capturing parens below */
#define  BACKR	 020			/* back reference below */
#define  INUSE	 0100			/* in use in final tree */
#define  NOPROP  03				/* bits which may not propagate up */
#define  LMIX(f) ((f)<<2)		/* LONGER -> MIXED */
#define  SMIX(f) ((f)<<1)		/* SHORTER -> MIXED */
#define  UP(f)	 (((f)&~NOPROP) | (LMIX(f) & SMIX(f) & MIXED))
#define  MESSY(f)	 ((f)&(MIXED|CAP|BACKR))
#define  PREF(f) ((f)&NOPROP)
#define  PREF2(f1, f2)	 ((PREF(f1) != 0) ? PREF(f1) : PREF(f2))
#define  COMBINE(f1, f2) (UP((f1)|(f2)) | PREF2(f1, f2))
	short		id;				/* ID of subre (1..ntree-1) */
	int			subno;			/* subexpression number for 'b' and '(', or
								 * LATYPE code for lookaround constraint */
	short		min;			/* min repetitions for iteration or backref */
	short		max;			/* max repetitions for iteration or backref */
	struct subre *left;			/* left child, if any (also freelist chain) */
	struct subre *right;		/* right child, if any */
	struct state *begin;		/* outarcs from here... */
	struct state *end;			/* ...ending in inarcs here */
	struct cnfa cnfa;			/* compacted NFA, if any */
	struct subre *chain;		/* for bookkeeping and error cleanup */
};



/*
 * table of function pointers for generic manipulation functions
 * A regex_t's re_fns points to one of these.
 */
struct fns
{
	void		FUNCPTR(free, (regex_t *));
	int			FUNCPTR(cancel_requested, (void));
	int			FUNCPTR(stack_too_deep, (void));
};

#define CANCEL_REQUESTED(re)  \
	((*((struct fns *) (re)->re_fns)->cancel_requested) ())

#define STACK_TOO_DEEP(re)	\
	((*((struct fns *) (re)->re_fns)->stack_too_deep) ())


/*
 * the insides of a regex_t, hidden behind a void *
 */
struct guts
{
	int			magic;
#define  GUTSMAGIC	 0xfed9
	int			cflags;			/* copy of compile flags */
	long		info;			/* copy of re_info */
	size_t		nsub;			/* copy of re_nsub */
	struct subre *tree;
	struct cnfa search;			/* for fast preliminary search */
	int			ntree;			/* number of subre's, plus one */
	struct colormap cmap;
	int			FUNCPTR(compare, (const chr *, const chr *, size_t));
	struct subre *lacons;		/* lookaround-constraint vector */
	int			nlacons;		/* size of lacons[]; note that only slots
								 * numbered 1 .. nlacons-1 are used */
};


/* prototypes for functions that are exported from regcomp.c to regexec.c */
extern void pg_set_regex_collation(Oid collation);
extern color pg_reg_getcolor(struct colormap *cm, chr c);
