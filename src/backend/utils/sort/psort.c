/*-------------------------------------------------------------------------
 *
 * psort.c--
 *    Polyphase merge sort.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/psort.c,v 1.2 1996/11/03 06:54:38 scrappy Exp $
 *
 * NOTES
 *	Sorts the first relation into the second relation.  The sort may
 * not be called twice simultaneously.
 *
 *    Use the tape-splitting method (Knuth, Vol. III, pp281-86) in the future.
 *
 *	Arguments? Variables?
 *		MAXMERGE, MAXTAPES
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <math.h>

#include "postgres.h"

#include "executor/execdebug.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "utils/tqual.h"	/* for NowTimeQual */

#include "storage/buf.h"
#include "storage/bufmgr.h"	/* for BLCKSZ */
#include "utils/portal.h"	/* for {Start,End}PortalAllocMode */
#include "utils/rel.h"

#include "utils/psort.h"
#include "utils/lselect.h"

#include "storage/fd.h"

#define	TEMPDIR	"./"

int			Nkeys;
ScanKey			Key;
int			SortMemory;

static	int		TapeRange;		/* number of tapes - 1 (T) */
static	int		Level;			/* (l) */
static	int		TotalDummy;		/* summation of tp_dummy */
static	struct	tape	Tape[MAXTAPES];
static	long		shortzero = 0;		/* used to delimit runs */
static	struct	tuple	*LastTuple = NULL;	/* last output */

static  int		BytesRead;		/* to keep track of # of IO */
static  int		BytesWritten;

Relation		SortRdesc;		/* current tuples in memory */
struct	leftist		*Tuples;		/* current tuples in memory */

/*
 *	psort		- polyphase merge sort entry point
 */
void
psort(Relation oldrel, Relation newrel, int nkeys, ScanKey key)
{
    AssertArg(nkeys >= 1);
    AssertArg(key[0].sk_attno != 0);
    AssertArg(key[0].sk_procedure != 0);
    
    Nkeys = nkeys;
    Key = key;
    SortMemory = 0;
    SortRdesc = oldrel;
    BytesRead = 0;
    BytesWritten = 0;
    /* 
     * may not be the best place.  
     *
     * Pass 0 for the "limit" as the argument is currently ignored.
     * Previously, only one arg was passed. -mer 12 Nov. 1991
     */
    StartPortalAllocMode(StaticAllocMode, (Size)0);
    initpsort();
    initialrun(oldrel);
    /* call finalrun(newrel, mergerun()) instead */
    endpsort(newrel, mergeruns());
    EndPortalAllocMode();
    NDirectFileRead += (int)ceil((double)BytesRead / BLCKSZ);
    NDirectFileWrite += (int)ceil((double)BytesWritten / BLCKSZ);
}

/*
 *	TAPENO		- number of tape in Tape
 */

#define	TAPENO(NODE)		(NODE - Tape)
#define	TUPLENO(TUP)		((TUP == NULL) ? -1 : (int) TUP->t_iid)

/*
 *	initpsort	- initializes the tapes
 *			- (polyphase merge Alg.D(D1)--Knuth, Vol.3, p.270)
 *	Returns:
 *		number of allocated tapes
 */
void
initpsort()
{
    register	int			i;
    register	struct	tape		*tp;
    
    /*
      ASSERT(ntapes >= 3 && ntapes <= MAXTAPES,
      "initpsort: Invalid number of tapes to initialize.\n");
      */
    
    tp = Tape;
    for (i = 0; i < MAXTAPES && (tp->tp_file = gettape()) != NULL; i++) {
	tp->tp_dummy = 1;
	tp->tp_fib = 1;
	tp->tp_prev = tp - 1;
	tp++;
    }
    TapeRange = --tp - Tape;
    tp->tp_dummy = 0;
    tp->tp_fib = 0;
    Tape[0].tp_prev = tp;
    
    if (TapeRange <= 1)
	elog(WARN, "initpsort: Could only allocate %d < 3 tapes\n",
	     TapeRange + 1);
    
    Level = 1;
    TotalDummy = TapeRange;
    
    SortMemory = SORTMEM;
    LastTuple = NULL;
    Tuples = NULL;
}

/*
 *	resetpsort	- resets (frees) malloc'd memory for an aborted Xaction
 *
 *	Not implemented yet.
 */
void
resetpsort()
{
    ;
}

/*
 *	PUTTUP		- writes the next tuple
 *	ENDRUN		- mark end of run
 *	GETLEN		- reads the length of the next tuple
 *	ALLOCTUP	- returns space for the new tuple
 *	SETTUPLEN	- stores the length into the tuple
 *	GETTUP		- reads the tuple
 *
 *	Note:
 *		LEN field must be a short; FP is a stream
 */

#define	PUTTUP(TUP, FP)\
    BytesWritten += (TUP)->t_len; \
    fwrite((char *)TUP, (TUP)->t_len, 1, FP)
#define	ENDRUN(FP)	fwrite((char *)&shortzero, sizeof (shortzero), 1, FP)
#define	GETLEN(LEN, FP)	fread((char *)&(LEN), sizeof (shortzero), 1, FP)
#define	ALLOCTUP(LEN)	((HeapTuple)malloc((unsigned)LEN))
#define	GETTUP(TUP, LEN, FP)\
    IncrProcessed(); \
    BytesRead += (LEN) - sizeof (shortzero); \
    fread((char *)(TUP) + sizeof (shortzero), (LEN) - sizeof (shortzero), 1, FP)
#define	SETTUPLEN(TUP, LEN)	(TUP)->t_len = LEN
    
    /*
     *	USEMEM		- record use of memory
     *	FREEMEM		- record freeing of memory
     *	FULLMEM		- 1 iff a tuple will fit
     */
    
#define	USEMEM(AMT)	SortMemory -= (AMT)
#define	FREEMEM(AMT)	SortMemory += (AMT)
#define	LACKMEM()	(SortMemory <= BLCKSZ)		/* not accurate */
#define	TRACEMEM(FUNC)
#define	TRACEOUT(FUNC, TUP)

/*
 *	initialrun	- distributes tuples from the relation
 *			- (replacement selection(R2-R3)--Knuth, Vol.3, p.257)
 *			- (polyphase merge Alg.D(D2-D4)--Knuth, Vol.3, p.271)
 *
 *	Explaination:
 *		Tuples are distributed to the tapes as in Algorithm D.
 *		A "tuple" with t_size == 0 is used to mark the end of a run.
 *
 *	Note:
 *		The replacement selection algorithm has been modified
 *		to go from R1 directly to R3 skipping R2 the first time.
 *
 *		Maybe should use closer(rdesc) before return
 *		Perhaps should adjust the number of tapes if less than n.
 *		used--v. likely to have problems in mergeruns().
 *		Must know if should open/close files before each
 *		call to  psort()?   If should--messy??
 *
 *	Possible optimization:
 *		put the first xxx runs in quickly--problem here since
 *		I (perhaps prematurely) combined the 2 algorithms.
 *		Also, perhaps allocate tapes when needed. Split into 2 funcs.
 */
void
initialrun(Relation rdesc)
{
    /*	register struct	tuple	*tup; */
    register struct	tape	*tp;
    HeapScanDesc	sdesc;
    int		baseruns;		/* D:(a) */
    int		morepasses;		/* EOF */
    
    sdesc = heap_beginscan(rdesc, 0, NowTimeQual, 0,
			   (ScanKey)NULL);
    tp = Tape;
    
    if ((bool)createrun(sdesc, tp->tp_file) != false)
	morepasses = 0;
    else
	morepasses = 1 + (Tuples != NULL); 	/* (T != N) ? 2 : 1 */
    
    for ( ; ; ) {
	tp->tp_dummy--;
	TotalDummy--;
	if (tp->tp_dummy < (tp + 1)->tp_dummy)
	    tp++;
	else if (tp->tp_dummy != 0)
	    tp = Tape;
	else {
	    Level++;
	    baseruns = Tape[0].tp_fib;
	    for (tp = Tape; tp - Tape < TapeRange; tp++) {
		TotalDummy +=
		    (tp->tp_dummy = baseruns
		     + (tp + 1)->tp_fib
		     - tp->tp_fib);
		tp->tp_fib = baseruns
		    + (tp + 1)->tp_fib;
	    }
	    tp = Tape;			/* D4 */
	}					/* D3 */
	if (morepasses)
	    if (--morepasses) {
		dumptuples(tp->tp_file);
		ENDRUN(tp->tp_file);
		continue;
	    } else
		break;
	if ((bool)createrun(sdesc, tp->tp_file) == false)
	    morepasses = 1 + (Tuples != NULL);
	/* D2 */
    }
    for (tp = Tape + TapeRange; tp >= Tape; tp--)
	rewind(tp->tp_file);				/* D. */
    heap_endscan(sdesc);
}

/*
 *	createrun	- places the next run on file
 *
 *	Uses:
 *		Tuples, which should contain any tuples for this run
 *
 *	Returns:
 *		FALSE iff process through end of relation
 *		Tuples contains the tuples for the following run upon exit
 */
bool
createrun(HeapScanDesc sdesc, FILE *file)
{
    register HeapTuple	lasttuple;
    register HeapTuple	btup, tup;
    struct	leftist	*nextrun;
    Buffer	b;
    bool		foundeor;
    short		junk;
    
    lasttuple = NULL;
    nextrun = NULL;
    foundeor = false;
    for ( ; ; ) {
	while (LACKMEM() && Tuples != NULL) {
	    if (lasttuple != NULL) {
		FREEMEM(lasttuple->t_len);
		FREE(lasttuple);
		TRACEMEM(createrun);
	    }
	    lasttuple = tup = gettuple(&Tuples, &junk);
	    PUTTUP(tup, file);
	    TRACEOUT(createrun, tup);
	}
	if (LACKMEM())
	    break;
	btup = heap_getnext(sdesc, 0, &b);
	if (!HeapTupleIsValid(btup)) {
	    foundeor = true;
	    break;
	}
	IncrProcessed();
	tup = tuplecopy(btup, sdesc->rs_rd, b);
	USEMEM(tup->t_len);
	TRACEMEM(createrun);
	if (lasttuple != NULL && tuplecmp(tup, lasttuple))
	    puttuple(&nextrun, tup, 0);
	else
	    puttuple(&Tuples, tup, 0);
	ReleaseBuffer(b);
    }
    if (lasttuple != NULL) {
	FREEMEM(lasttuple->t_len);
	FREE(lasttuple);
	TRACEMEM(createrun);
    }
    dumptuples(file);
    ENDRUN(file);
    /* delimit the end of the run */
    Tuples = nextrun;
    return((bool)! foundeor); /* XXX - works iff bool is {0,1} */
}

/*
 *	tuplecopy	- see also tuple.c:palloctup()
 *
 *	This should eventually go there under that name?  And this will
 *	then use malloc directly (see version -r1.2).
 */
HeapTuple
tuplecopy(HeapTuple tup, Relation rdesc, Buffer b)
{
    HeapTuple	rettup;
    
    if (!HeapTupleIsValid(tup)) {
	return(NULL);		/* just in case */
    }
    rettup = (HeapTuple)malloc(tup->t_len);
    memmove((char *)rettup, (char *)tup, tup->t_len);	/* XXX */
    return(rettup);
}

/*
 *	mergeruns	- merges all runs from input tapes
 *			  (polyphase merge Alg.D(D6)--Knuth, Vol.3, p271)
 *
 *	Returns:
 *		file of tuples in order
 */
FILE *
mergeruns()
{
    register struct	tape	*tp;
    
    tp = Tape + TapeRange;
    merge(tp);
    rewind(tp->tp_file);
    while (--Level != 0) {
	tp = tp->tp_prev;
	rewind(tp->tp_file);
	/*		resettape(tp->tp_file);		-not sufficient */
	merge(tp);
	rewind(tp->tp_file);
    }
    return(tp->tp_file);
}

/*
 *	merge		- handles a single merge of the tape
 *			  (polyphase merge Alg.D(D5)--Knuth, Vol.3, p271)
 */
void
merge(struct tape *dest)
{
    register HeapTuple	tup;
    register struct	tape	*lasttp;	/* (TAPE[P]) */
    register struct	tape	*tp;
    struct	leftist	*tuples;
    FILE		*destfile;
    int		times;		/* runs left to merge */
    int		outdummy;	/* complete dummy runs */
    short		fromtape;
    long		tuplen;
    
    lasttp = dest->tp_prev;
    times = lasttp->tp_fib;
    for (tp = lasttp ; tp != dest; tp = tp->tp_prev)
	tp->tp_fib -= times;
    tp->tp_fib += times;
    /* Tape[].tp_fib (A[]) is set to proper exit values */
    
    if (TotalDummy < TapeRange)		/* no complete dummy runs */
	outdummy = 0;
    else {
	outdummy = TotalDummy;		/* a large positive number */
	for (tp = lasttp; tp != dest; tp = tp->tp_prev)
	    if (outdummy > tp->tp_dummy)
		outdummy = tp->tp_dummy;
	for (tp = lasttp; tp != dest; tp = tp->tp_prev)
	    tp->tp_dummy -= outdummy;
	tp->tp_dummy += outdummy;
	TotalDummy -= outdummy * TapeRange;
	/* do not add the outdummy runs yet */
	times -= outdummy;
    }
    destfile = dest->tp_file;
    while (times-- != 0) {			/* merge one run */
	tuples = NULL;
	if (TotalDummy == 0)
	    for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev) {
		GETLEN(tuplen, tp->tp_file);
		tup = ALLOCTUP(tuplen);
		USEMEM(tuplen);
		TRACEMEM(merge);
		SETTUPLEN(tup, tuplen);
		GETTUP(tup, tuplen, tp->tp_file);
		puttuple(&tuples, tup, TAPENO(tp));
	    }
	else {
	    for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev) {
		if (tp->tp_dummy != 0) {
		    tp->tp_dummy--;
		    TotalDummy--;
		} else {
		    GETLEN(tuplen, tp->tp_file);
		    tup = ALLOCTUP(tuplen);
		    USEMEM(tuplen);
		    TRACEMEM(merge);
		    SETTUPLEN(tup, tuplen);
		    GETTUP(tup, tuplen, tp->tp_file);
		    puttuple(&tuples, tup, TAPENO(tp));
		}
	    }
	}
	while (tuples != NULL) {
	    /* possible optimization by using count in tuples */
	    tup = gettuple(&tuples, &fromtape);
	    PUTTUP(tup, destfile);
	    FREEMEM(tup->t_len);
	    FREE(tup);
	    TRACEMEM(merge);
	    GETLEN(tuplen, Tape[fromtape].tp_file);
	    if (tuplen == 0)
		;
	    else {
		tup = ALLOCTUP(tuplen);
		USEMEM(tuplen);
		TRACEMEM(merge);
		SETTUPLEN(tup, tuplen);
		GETTUP(tup, tuplen, Tape[fromtape].tp_file);
		puttuple(&tuples, tup, fromtape);
	    }
	}				
	ENDRUN(destfile);
    }
    TotalDummy += outdummy;
}

/*
 *	endpsort	- creates the new relation and unlinks the tape files
 */
void
endpsort(Relation rdesc, FILE *file)
{
    register struct	tape	*tp;
    register HeapTuple	tup;
    long		tuplen;
    
    if (! feof(file))
	while (GETLEN(tuplen, file) && tuplen != 0) {
	    tup = ALLOCTUP(tuplen);
	    SortMemory += tuplen;
	    SETTUPLEN(tup, tuplen);
	    GETTUP(tup, tuplen, file);
	    heap_insert(rdesc, tup);
	    FREE(tup);
	    SortMemory -= tuplen;
	}
    for (tp = Tape + TapeRange; tp >= Tape; tp--)
	destroytape(tp->tp_file);
}

/*
 *	gettape		- handles access temporary files in polyphase merging
 *
 *	Optimizations:
 *		If guarenteed that only one sort running/process,
 *		can simplify the file generation--and need not store the
 *		name for later unlink.
 */

struct tapelst {
    char		*tl_name;
    int		tl_fd;
    struct	tapelst	*tl_next;
};

static struct	tapelst	*Tapes = NULL;
static char	Tempfile[MAXPGPATH] = TEMPDIR;

/*
 *	gettape		- returns an open stream for writing/reading
 *
 *	Returns:
 *		Open stream for writing/reading.
 *		NULL if unable to open temporary file.
 */
FILE *
gettape()
{
    register struct	tapelst	*tp;
    FILE		*file;
    static	int	tapeinit = 0;
    char		*mktemp();
    
    tp = (struct tapelst *)malloc((unsigned)sizeof (struct tapelst));
    if (!tapeinit) {
	Tempfile[sizeof (TEMPDIR) - 1] = '/';
	memmove(Tempfile + sizeof(TEMPDIR), TAPEEXT, sizeof (TAPEEXT));
	tapeinit = 1;
    }
    tp->tl_name = malloc((unsigned)sizeof(Tempfile));
    /*
     * now, copy template with final null into malloc'd space
     */
    memmove(tp->tl_name, Tempfile, sizeof (TEMPDIR) + sizeof (TAPEEXT));
    mktemp(tp->tl_name);
    
    AllocateFile();
    file = fopen(tp->tl_name, "w+");
    if (file == NULL) {
	/* XXX this should not happen */
	FreeFile();
	FREE(tp->tl_name);
	FREE(tp);
	return(NULL);
    }
    
    tp->tl_fd = fileno(file);
    tp->tl_next = Tapes;
    Tapes = tp;
    return(file);
}

/*
 *	resettape	- resets the tape to size 0
 */
void
resettape(FILE *file)
{
    register	struct	tapelst	*tp;
    register	int		fd;
    
    Assert(PointerIsValid(file));
    
    fd = fileno(file);
    for (tp = Tapes; tp != NULL && tp->tl_fd != fd; tp = tp->tl_next)
	;
    if (tp == NULL)
	elog(WARN, "resettape: tape not found");
    
    file = freopen(tp->tl_name, "w+", file);
    if (file == NULL) {
	elog(FATAL, "could not freopen temporary file");
    }
}

/*
 *	distroytape	- unlinks the tape
 *
 *	Efficiency note:
 *		More efficient to destroy more recently allocated tapes first.
 *
 *	Possible bugs:
 *		Exits instead of returning status, if given invalid tape.
 */
void
destroytape(FILE *file)
{
    register	struct	tapelst		*tp, *tq;
    register	int			fd;
    
    if ((tp = Tapes) == NULL)
	elog(FATAL, "destroytape: tape not found");
    
    if ((fd = fileno(file)) == tp->tl_fd) {
	Tapes = tp->tl_next;
	fclose(file);
	FreeFile();
	unlink(tp->tl_name);
	FREE(tp->tl_name);
	FREE(tp);
    } else
	for ( ; ; ) {
	    if (tp->tl_next == NULL)
		elog(FATAL, "destroytape: tape not found");
	    if (tp->tl_next->tl_fd == fd) {
		fclose(file);
		FreeFile();
		tq = tp->tl_next;
		tp->tl_next = tq->tl_next;
		unlink(tq->tl_name);
		FREE((tq->tl_name));
		FREE(tq);
		break;
	    }
	    tp = tp->tl_next;
	}
}
