/*-------------------------------------------------------------------------
 *
 * psort.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: psort.h,v 1.3 1997/05/20 11:37:33 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PSORT_H
#define	PSORT_H

#include <stdio.h>
#include <access/relscan.h>

#define	SORTMEM		(1 << 18)		/* 1/4 M - any static memory */
#define	MAXTAPES	7			/* 7--See Fig. 70, p273 */
#define	TAPEEXT		"pg_psort.XXXXXX"	/* TEMPDIR/TAPEEXT */
#define	FREE(x)		pfree((char *) x)

struct	tape {
    int		tp_dummy;	/* (D) */
    int		tp_fib;		/* (A) */
    FILE	*tp_file; 	/* (TAPE) */
    struct tape	*tp_prev;
};

struct	cmplist {
    int		cp_attn; 	/* attribute number */
    int		cp_num;		/* comparison function code */
    int		cp_rev;		/* invert comparison flag */
    struct	cmplist		*cp_next; /* next in chain */
};

extern	int		Nkeys;
extern	ScanKey		key;
extern	int		SortMemory;	/* free memory */
extern	Relation	SortRdesc;
extern	struct leftist	*Tuples;

#ifdef	EBUG

#define	PDEBUG(PROC, S1)\
elog(DEBUG, "%s:%d>> PROC: %s.", __FILE__, __LINE__, S1)

#define	PDEBUG2(PROC, S1, D1)\
elog(DEBUG, "%s:%d>> PROC: %s %d.", __FILE__, __LINE__, S1, D1)

#define	PDEBUG4(PROC, S1, D1, S2, D2)\
elog(DEBUG, "%s:%d>> PROC: %s %d, %s %d.", __FILE__, __LINE__, S1, D1, S2, D2)

#define	VDEBUG(VAR, FMT)\
elog(DEBUG, "%s:%d>> VAR =FMT", __FILE__, __LINE__, VAR)

#define	ASSERT(EXPR, STR)\
if (!(EXPR)) elog(FATAL, "%s:%d>> %s", __FILE__, __LINE__, STR)

#define	TRACE(VAL, CODE)\
if (1) CODE; else

#else
#define	PDEBUG(MSG)
#define	VDEBUG(VAR, FMT)
#define	ASSERT(EXPR, MSG)
#define	TRACE(VAL, CODE)
#endif

/* psort.c */
extern void psort(Relation oldrel, Relation newrel, int nkeys, ScanKey key);
extern void initpsort(void);
extern void resetpsort(void);
extern void initialrun(Relation rdesc);
extern bool createrun(HeapScanDesc sdesc, FILE *file);
extern HeapTuple tuplecopy(HeapTuple tup, Relation rdesc, Buffer b);
extern FILE *mergeruns(void);
extern void merge(struct tape *dest);
extern void endpsort(Relation rdesc, FILE *file);
extern FILE *gettape(void);
extern void resettape(FILE *file);
extern void destroytape(FILE *file);

#endif	/* PSORT_H */
