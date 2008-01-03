/*
 * Internal interface definitions, etc., for the reg package
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
 * $PostgreSQL: pgsql/src/include/regex/regguts.h,v 1.6 2008/01/03 20:47:55 tgl Exp $
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
#ifndef CHAR_BIT
#include <limits.h>
#endif
#ifndef _POSIX2_RE_DUP_MAX
#define _POSIX2_RE_DUP_MAX	255 /* normally from <limits.h> */
#endif



/*
 * misc
 */

#define NOTREACHED	0
#define xxx		1

#define DUPMAX	_POSIX2_RE_DUP_MAX
#define INFINITY	(DUPMAX+1)

#define REMAGIC 0xfed7			/* magic number for main struct */



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
 * We dissect a chr into byts for colormap table indexing.	Here we define
 * a byt, which will be the same as a byte on most machines...	The exact
 * size of a byt is not critical, but about 8 bits is good, and extraction
 * of 8-bit chunks is sometimes especially fast.
 */
#ifndef BYTBITS
#define BYTBITS 8				/* bits in a byt */
#endif
#define BYTTAB	(1<<BYTBITS)	/* size of table with one entry per byt value */
#define BYTMASK (BYTTAB-1)		/* bit mask for byt */
#define NBYTS	((CHRBITS+BYTBITS-1)/BYTBITS)
/* the definition of GETCOLOR(), below, assumes NBYTS <= 4 */



/*
 * As soon as possible, we map chrs into equivalence classes -- "colors" --
 * which are of much more manageable number.
 */
typedef short color;			/* colors of characters */
typedef int pcolor;				/* what color promotes to */

#define COLORLESS	(-1)		/* impossible color */
#define WHITE		0			/* default color, parent of all others */



/*
 * A colormap is a tree -- more precisely, a DAG -- indexed at each level
 * by a byt of the chr, to map the chr to a color efficiently.	Because
 * lower sections of the tree can be shared, it can exploit the usual
 * sparseness of such a mapping table.	The tree is always NBYTS levels
 * deep (in the past it was shallower during construction but was "filled"
 * to full depth at the end of that); areas that are unaltered as yet point
 * to "fill blocks" which are entirely WHITE in color.
 */

/* the tree itself */
struct colors
{
	color		ccolor[BYTTAB];
};
struct ptrs
{
	union tree *pptr[BYTTAB];
};
union tree
{
	struct colors colors;
	struct ptrs ptrs;
};

#define tcolor	colors.ccolor
#define tptr	ptrs.pptr

/* internal per-color structure for the color machinery */
struct colordesc
{
	uchr		nchrs;			/* number of chars of this color */
	color		sub;			/* open subcolor (if any); free chain ptr */
#define  NOSUB	 COLORLESS
	struct arc *arcs;			/* color chain */
	int			flags;
#define  FREECOL 01				/* currently free */
#define  PSEUDO  02				/* pseudocolor, no real chars */
#define  UNUSEDCOLOR(cd) ((cd)->flags&FREECOL)
	union tree *block;			/* block of solid color, if any */
};

/* the color map itself */
struct colormap
{
	int			magic;
#define  CMMAGIC 0x876
	struct vars *v;				/* for compile error reporting */
	size_t		ncds;			/* number of colordescs */
	size_t		max;			/* highest in use */
	color		free;			/* beginning of free chain (if non-0) */
	struct colordesc *cd;
#define  CDEND(cm)	 (&(cm)->cd[(cm)->max + 1])
#define  NINLINECDS  ((size_t)10)
	struct colordesc cdspace[NINLINECDS];
	union tree	tree[NBYTS];	/* tree top, plus fill blocks */
};

/* optimization magic to do fast chr->color mapping */
#define B0(c)	((c) & BYTMASK)
#define B1(c)	(((c)>>BYTBITS) & BYTMASK)
#define B2(c)	(((c)>>(2*BYTBITS)) & BYTMASK)
#define B3(c)	(((c)>>(3*BYTBITS)) & BYTMASK)
#if NBYTS == 1
#define GETCOLOR(cm, c) ((cm)->tree->tcolor[B0(c)])
#endif
/* beware, for NBYTS>1, GETCOLOR() is unsafe -- 2nd arg used repeatedly */
#if NBYTS == 2
#define GETCOLOR(cm, c) ((cm)->tree->tptr[B1(c)]->tcolor[B0(c)])
#endif
#if NBYTS == 4
#define GETCOLOR(cm, c) ((cm)->tree->tptr[B3(c)]->tptr[B2(c)]->tptr[B1(c)]->tcolor[B0(c)])
#endif



/*
 * Interface definitions for locale-interface functions in locale.c.
 * Multi-character collating elements (MCCEs) cause most of the trouble.
 */
struct cvec
{
	int			nchrs;			/* number of chrs */
	int			chrspace;		/* number of chrs possible */
	chr		   *chrs;			/* pointer to vector of chrs */
	int			nranges;		/* number of ranges (chr pairs) */
	int			rangespace;		/* number of chrs possible */
	chr		   *ranges;			/* pointer to vector of chr pairs */
	int			nmcces;			/* number of MCCEs */
	int			mccespace;		/* number of MCCEs possible */
	int			nmccechrs;		/* number of chrs used for MCCEs */
	chr		   *mcces[1];		/* pointers to 0-terminated MCCEs */
	/* and both batches of chrs are on the end */
};

/* caution:  this value cannot be changed easily */
#define MAXMCCE 2				/* length of longest MCCE */



/*
 * definitions for NFA internal representation
 *
 * Having a "from" pointer within each arc may seem redundant, but it
 * saves a lot of hassle.
 */
struct state;

struct arc
{
	int			type;
#define  ARCFREE '\0'
	color		co;
	struct state *from;			/* where it's from (and contained within) */
	struct state *to;			/* where it's to */
	struct arc *outchain;		/* *from's outs chain or free chain */
#define  freechain	 outchain
	struct arc *inchain;		/* *to's ins chain */
	struct arc *colorchain;		/* color's arc chain */
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
	size_t		size;			/* Current NFA size; differs from nstates as
								 * it also counts the number of states created
								 * by children of this state. */
	struct vars *v;				/* simplifies compile error reporting */
	struct nfa *parent;			/* parent NFA, if any */
};



/*
 * definitions for compacted NFA
 */
struct carc
{
	color		co;				/* COLORLESS is list terminator */
	int			to;				/* state number */
};

struct cnfa
{
	int			nstates;		/* number of states */
	int			ncolors;		/* number of colors */
	int			flags;
#define  HASLACONS	 01			/* uses lookahead constraints */
	int			pre;			/* setup state number */
	int			post;			/* teardown state number */
	color		bos[2];			/* colors, if any, assigned to BOS and BOL */
	color		eos[2];			/* colors, if any, assigned to EOS and EOL */
	struct carc **states;		/* vector of pointers to outarc lists */
	struct carc *arcs;			/* the area for the lists */
};

#define ZAPCNFA(cnfa)	((cnfa).nstates = 0)
#define NULLCNFA(cnfa)	((cnfa).nstates == 0)

/*
 * Used to limit the maximum NFA size to something sane. [Tcl Bug 1810264]
 */
#ifndef REG_MAX_STATES
#define REG_MAX_STATES	100000
#endif

/*
 * subexpression tree
 */
struct subre
{
	char		op;				/* '|', '.' (concat), 'b' (backref), '(', '=' */
	char		flags;
#define  LONGER  01				/* prefers longer match */
#define  SHORTER 02				/* prefers shorter match */
#define  MIXED	 04				/* mixed preference below */
#define  CAP 010				/* capturing parens below */
#define  BACKR	 020			/* back reference below */
#define  INUSE	 0100			/* in use in final tree */
#define  LOCAL	 03				/* bits which may not propagate up */
#define  LMIX(f) ((f)<<2)		/* LONGER -> MIXED */
#define  SMIX(f) ((f)<<1)		/* SHORTER -> MIXED */
#define  UP(f)	 (((f)&~LOCAL) | (LMIX(f) & SMIX(f) & MIXED))
#define  MESSY(f)	 ((f)&(MIXED|CAP|BACKR))
#define  PREF(f) ((f)&LOCAL)
#define  PREF2(f1, f2)	 ((PREF(f1) != 0) ? PREF(f1) : PREF(f2))
#define  COMBINE(f1, f2) (UP((f1)|(f2)) | PREF2(f1, f2))
	short		retry;			/* index into retry memory */
	int			subno;			/* subexpression number (for 'b' and '(') */
	short		min;			/* min repetitions, for backref only */
	short		max;			/* max repetitions, for backref only */
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
};



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
	int			ntree;
	struct colormap cmap;
	int			FUNCPTR(compare, (const chr *, const chr *, size_t));
	struct subre *lacons;		/* lookahead-constraint vector */
	int			nlacons;		/* size of lacons */
};
