/*-------------------------------------------------------------------------
 *
 * psort.c--
 *    Polyphase merge sort.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/psort.c,v 1.16 1997/08/18 02:14:56 momjian Exp $
 *
 * NOTES
 *      Sorts the first relation into the second relation.
 *
 *      The old psort.c's routines formed a temporary relation from the merged
 * sort files. This version keeps the files around instead of generating the
 * relation from them, and provides interface functions to the file so that
 * you can grab tuples, mark a position in the file, restore a position in the
 * file. You must now explicitly call an interface function to end the sort,
 * psort_end, when you are done.
 *      Now most of the global variables are stuck in the Sort nodes, and
 * accessed from there (they are passed to all the psort routines) so that
 * each sort running has its own separate state. This is facilitated by having
 * the Sort nodes passed in to all the interface functions.
 *      The one global variable that all the sorts still share is SortMemory.
 *      You should now be allowed to run two or more psorts concurrently,
 * so long as the memory they eat up is not greater than SORTMEM, the initial
 * value of SortMemory.				        	-Rex 2.15.1995
 *
 *    Use the tape-splitting method (Knuth, Vol. III, pp281-86) in the future.
 *
 *	Arguments? Variables?
 *		MAXMERGE, MAXTAPES
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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
#include "utils/elog.h"
#include "utils/rel.h"

#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "executor/executor.h"

#include "utils/lselect.h"
#include "utils/psort.h"

#include "miscadmin.h"
#include "storage/fd.h"

#define	TEMPDIR	"./"

static  	long	shortzero = 0;	/* used to delimit runs */

/*
 * old psort global variables
 *
 * (These are the global variables from the old psort. They are still used,
 *  but are now accessed from Sort nodes using the PS macro. Note that while
 *  these variables will be accessed by PS(node)->whatever, they will still
 *  be called by their original names within the comments!      -Rex 2.10.1995)
 *
 * LeftistContextData   treeContext;
 *
 * static       int     TapeRange;		// number of tapes - 1 (T) //
 * static       int     Level;			// (l) //
 * static       int     TotalDummy;		// summation of tp_dummy //
 * static struct tape   *Tape;
 *
 * static  	int	BytesRead;		// to keep track of # of IO //
 * static  	int	BytesWritten;
 *
 * struct leftist       *Tuples;		// current tuples in memory //
 *
 * FILE		 	*psort_grab_file;       // this holds tuples grabbed
 *						   from merged sort runs //
 * long		 	psort_current;		// current file position //
 * long		 	psort_saved;		// file position saved for
 *						   mark and restore //
 */

/*
 * PS - Macro to access and cast psortstate from a Sort node
 */
#define PS(N) ((Psortstate *)N->psortstate)

/*
 *      psort_begin     - polyphase merge sort entry point. Sorts the subplan
 *			  into a temporary file psort_grab_file. After
 *			  this is called, calling the interface function
 *			  psort_grabtuple iteratively will get you the sorted
 *			  tuples. psort_end then finishes the sort off, after
 *			  all the tuples have been grabbed.
 *
 *		          Allocates and initializes sort node's psort state.
 */
bool
psort_begin(Sort *node, int nkeys, ScanKey key)
{
    bool empty;	 /* to answer: is child node empty? */

    node->psortstate = (struct Psortstate *)palloc(sizeof(struct Psortstate));
    if (node->psortstate == NULL)
	return false;

    AssertArg(nkeys >= 1);
    AssertArg(key[0].sk_attno != 0);
    AssertArg(key[0].sk_procedure != 0);
    
    PS(node)->BytesRead = 0;
    PS(node)->BytesWritten = 0;
    PS(node)->treeContext.tupDesc =
	ExecGetTupType(outerPlan((Plan *)node));
    PS(node)->treeContext.nKeys = nkeys;
    PS(node)->treeContext.scanKeys = key;
    PS(node)->treeContext.sortMem = SortMem * 1024;

    PS(node)->Tuples = NULL;
    PS(node)->tupcount = 0;
    
    PS(node)->using_tape_files = false;
    PS(node)->memtuples = NULL;

    initialrun(node, &empty);

    if (empty)
	return false;

    if (PS(node)->using_tape_files)
	PS(node)->psort_grab_file = mergeruns(node);

    PS(node)->psort_current = 0;
    PS(node)->psort_saved = 0;

    return true;
}

/*
 *	inittapes	- initializes the tapes
 *			- (polyphase merge Alg.D(D1)--Knuth, Vol.3, p.270)
 *	Returns:
 *		number of allocated tapes
 */
void
inittapes(Sort *node)
{
    register	int			i;
    register	struct	tape		*tp;
    
    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    /*
      ASSERT(ntapes >= 3 && ntapes <= MAXTAPES,
      "inittapes: Invalid number of tapes to initialize.\n");
      */
    
    tp = PS(node)->Tape;
    for (i = 0; i < MAXTAPES && (tp->tp_file = gettape()) != NULL; i++) {
	tp->tp_dummy = 1;
	tp->tp_fib = 1;
	tp->tp_prev = tp - 1;
	tp++;
    }
    PS(node)->TapeRange = --tp - PS(node)->Tape;
    tp->tp_dummy = 0;
    tp->tp_fib = 0;
    PS(node)->Tape[0].tp_prev = tp;
    
    if (PS(node)->TapeRange <= 1)
	elog(WARN, "inittapes: Could only allocate %d < 3 tapes\n",
	     PS(node)->TapeRange + 1);
    
    PS(node)->Level = 1;
    PS(node)->TotalDummy = PS(node)->TapeRange;

    PS(node)->using_tape_files = true;
}

/*
 *	resetpsort	- resets (pfrees) palloc'd memory for an aborted Xaction
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


#define	PUTTUP(NODE, TUP, FP) do {\
    ((Psortstate *)NODE->psortstate)->BytesWritten += (TUP)->t_len; \
    fwrite((char *)TUP, (TUP)->t_len, 1, FP);} while (0)
#define	ENDRUN(FP)	fwrite((char *)&shortzero, sizeof (shortzero), 1, FP)
#define	GETLEN(LEN, FP)	fread((char *)&(LEN), sizeof (shortzero), 1, FP)
#define	ALLOCTUP(LEN)	((HeapTuple)palloc((unsigned)LEN))
#define	GETTUP(NODE, TUP, LEN, FP) do {\
    IncrProcessed(); \
    ((Psortstate *)NODE->psortstate)->BytesRead += (LEN) - sizeof (shortzero); \
    fread((char *)(TUP) + sizeof (shortzero), (LEN) - sizeof (shortzero), 1, FP);} \
				while (0)
#define	SETTUPLEN(TUP, LEN)	(TUP)->t_len = LEN
    
    /*
     *	USEMEM		- record use of memory
     *	FREEMEM		- record freeing of memory
     *	FULLMEM		- 1 iff a tuple will fit
     */
    
#define	USEMEM(NODE,AMT)	PS(node)->treeContext.sortMem -= (AMT)
#define	FREEMEM(NODE,AMT)	PS(node)->treeContext.sortMem += (AMT)
#define	LACKMEM(NODE)		(PS(node)->treeContext.sortMem <= MAXBLCKSZ)	/* not accurate */
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
initialrun(Sort *node, bool *empty)
{
    /*	register struct	tuple	*tup; */
    register struct	tape	*tp;
    int		baseruns;		/* D:(a) */
    int		extrapasses;		/* EOF */

    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    tp = PS(node)->Tape;
    
    if ((bool)createrun(node, NULL, empty) != false) {
	if (! PS(node)->using_tape_files)
	    inittapes(node);
	extrapasses = 0;
    }
    else {
	/* if empty or rows fit in memory, we never access tape stuff */
	if (*empty || ! PS(node)->using_tape_files)
	    return;
	if (! PS(node)->using_tape_files)
	    inittapes(node);
        extrapasses = 1 + (PS(node)->Tuples != NULL);   /* (T != N) ? 2 : 1 */
    }
    
    for ( ; ; ) {
	tp->tp_dummy--;
	PS(node)->TotalDummy--;
	if (tp->tp_dummy < (tp + 1)->tp_dummy)
	    tp++;
	else if (tp->tp_dummy != 0)
	    tp = PS(node)->Tape;
	else {
	    PS(node)->Level++;
	    baseruns = PS(node)->Tape[0].tp_fib;
	    for (tp = PS(node)->Tape;
		 tp - PS(node)->Tape < PS(node)->TapeRange; tp++) {
		PS(node)->TotalDummy +=
		    (tp->tp_dummy = baseruns
		     + (tp + 1)->tp_fib
		     - tp->tp_fib);
		tp->tp_fib = baseruns
		    + (tp + 1)->tp_fib;
	    }
	    tp = PS(node)->Tape;		/* D4 */
	}					/* D3 */
	if (extrapasses)
	    if (--extrapasses) {
		dumptuples(tp->tp_file, node);
		ENDRUN(tp->tp_file);
		continue;
	    } else
		break;

	if ((bool)createrun(node, tp->tp_file, empty) == false)
	    extrapasses = 1 + (PS(node)->Tuples != NULL);
	/* D2 */
    }
    for (tp = PS(node)->Tape + PS(node)->TapeRange; tp >= PS(node)->Tape; tp--)
	rewind(tp->tp_file);			    /* D. */
}

/*
 *      createrun       - places the next run on file, grabbing the tuples by
 *			executing the subplan passed in
 *
 *	Uses:
 *		Tuples, which should contain any tuples for this run
 *
 *	Returns:
 *		FALSE iff process through end of relation
 *		Tuples contains the tuples for the following run upon exit
 */
bool
createrun(Sort *node, FILE *file, bool *empty)
{
    register HeapTuple	lasttuple;
    register HeapTuple	tup;
    struct	leftist	*nextrun;
    bool		foundeor;
    short		junk;

    int cr_tuples = 0; /* Count tuples grabbed from plannode */
    TupleTableSlot *cr_slot;

    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    lasttuple = NULL;
    nextrun = NULL;
    foundeor = false;
    for ( ; ; ) {
	while (LACKMEM(node) && PS(node)->Tuples != NULL) {
	    if (lasttuple != NULL) {
		FREEMEM(node,lasttuple->t_len);
		FREE(lasttuple);
		TRACEMEM(createrun);
	    }
	    lasttuple = tup = gettuple(&PS(node)->Tuples, &junk,
				       &PS(node)->treeContext);
	    if (! PS(node)->using_tape_files) {
		inittapes(node);
		if (! file)
		    file = PS(node)->Tape->tp_file; /* was NULL */
	    }
	    PUTTUP(node, tup, file);
	    TRACEOUT(createrun, tup);
	}
	if (LACKMEM(node))
	    break;

	/* About to call ExecProcNode, it can mess up the state if it
	 * eventually calls another Sort node. So must stow it away here for
	 * the meantime.					-Rex 2.2.1995
	 */

	cr_slot = ExecProcNode(outerPlan((Plan *)node), (Plan *)node);

	if (TupIsNull(cr_slot)) {
	    foundeor = true;
	    break;
	}
	else {
	    tup = tuplecopy(cr_slot->val);
	    ExecClearTuple(cr_slot);
	    PS(node)->tupcount++;
	    cr_tuples++;
	}

	IncrProcessed();
	USEMEM(node,tup->t_len);
	TRACEMEM(createrun);
	if (lasttuple != NULL && tuplecmp(tup, lasttuple,
					  &PS(node)->treeContext))
	    puttuple(&nextrun, tup, 0, &PS(node)->treeContext);
	else
	    puttuple(&PS(node)->Tuples, tup, 0, &PS(node)->treeContext);
    }
    if (lasttuple != NULL) {
	FREEMEM(node,lasttuple->t_len);
	FREE(lasttuple);
	TRACEMEM(createrun);
    }
    dumptuples(file, node);
    if (PS(node)->using_tape_files)
	ENDRUN(file);
    /* delimit the end of the run */
    PS(node)->Tuples = nextrun;

    /* if we did not see any tuples, mark empty */
    *empty = (cr_tuples > 0) ? false : true;

    return((bool)! foundeor); /* XXX - works iff bool is {0,1} */
}

/*
 *	tuplecopy	- see also tuple.c:palloctup()
 *
 *	This should eventually go there under that name?  And this will
 *	then use palloc directly (see version -r1.2).
 */
HeapTuple
tuplecopy(HeapTuple tup)
{
    HeapTuple	rettup;
    
    if (!HeapTupleIsValid(tup)) {
	return(NULL);		/* just in case */
    }
    rettup = (HeapTuple)palloc(tup->t_len);
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
mergeruns(Sort *node)
{
    register struct     tape    *tp;

    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);
    Assert(PS(node)->using_tape_files == true);

    tp = PS(node)->Tape + PS(node)->TapeRange;
    merge(node, tp);
    rewind(tp->tp_file);
    while (--PS(node)->Level != 0) {
	tp = tp->tp_prev;
	rewind(tp->tp_file);
	/*	      resettape(tp->tp_file);	 -not sufficient */
	merge(node, tp);
	rewind(tp->tp_file);
    }
    return(tp->tp_file);
}

/*
 *	merge		- handles a single merge of the tape
 *			  (polyphase merge Alg.D(D5)--Knuth, Vol.3, p271)
 */
void
merge(Sort *node, struct tape *dest)
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
    
    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);
    Assert(PS(node)->using_tape_files == true);

    lasttp = dest->tp_prev;
    times = lasttp->tp_fib;
    for (tp = lasttp ; tp != dest; tp = tp->tp_prev)
	tp->tp_fib -= times;
    tp->tp_fib += times;
    /* Tape[].tp_fib (A[]) is set to proper exit values */
    
    if (PS(node)->TotalDummy < PS(node)->TapeRange)/* no complete dummy runs */
	outdummy = 0;
    else {
	outdummy = PS(node)->TotalDummy;	/* a large positive number */
	for (tp = lasttp; tp != dest; tp = tp->tp_prev)
	    if (outdummy > tp->tp_dummy)
		outdummy = tp->tp_dummy;
	for (tp = lasttp; tp != dest; tp = tp->tp_prev)
	    tp->tp_dummy -= outdummy;
	tp->tp_dummy += outdummy;
	PS(node)->TotalDummy -= outdummy * PS(node)->TapeRange;
	/* do not add the outdummy runs yet */
	times -= outdummy;
    }
    destfile = dest->tp_file;
    while (times-- != 0) {		      /* merge one run */
	tuples = NULL;
	if (PS(node)->TotalDummy == 0)
	    for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev) {
		GETLEN(tuplen, tp->tp_file);
		tup = ALLOCTUP(tuplen);
		USEMEM(node,tuplen);
		TRACEMEM(merge);
		SETTUPLEN(tup, tuplen);
		GETTUP(node, tup, tuplen, tp->tp_file);
		puttuple(&tuples, tup, tp - PS(node)->Tape,
			 &PS(node)->treeContext);
	    }
	else {
	    for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev) {
		if (tp->tp_dummy != 0) {
		    tp->tp_dummy--;
		    PS(node)->TotalDummy--;
		} else {
		    GETLEN(tuplen, tp->tp_file);
		    tup = ALLOCTUP(tuplen);
		    USEMEM(node,tuplen);
		    TRACEMEM(merge);
		    SETTUPLEN(tup, tuplen);
		    GETTUP(node, tup, tuplen, tp->tp_file);
		    puttuple(&tuples, tup, tp - PS(node)->Tape,
			     &PS(node)->treeContext);
		}
	    }
	}
	while (tuples != NULL) {
	    /* possible optimization by using count in tuples */
	    tup = gettuple(&tuples, &fromtape, &PS(node)->treeContext);
	    PUTTUP(node, tup, destfile);
	    FREEMEM(node,tup->t_len);
	    FREE(tup);
	    TRACEMEM(merge);
	    GETLEN(tuplen, PS(node)->Tape[fromtape].tp_file);
	    if (tuplen == 0)
		;
	    else {
		tup = ALLOCTUP(tuplen);
		USEMEM(node,tuplen);
		TRACEMEM(merge);
		SETTUPLEN(tup, tuplen);
		GETTUP(node, tup, tuplen, PS(node)->Tape[fromtape].tp_file);
		puttuple(&tuples, tup, fromtape, &PS(node)->treeContext);
	    }
	} 
	ENDRUN(destfile);
    }
    PS(node)->TotalDummy += outdummy;
}

/*
 * dumptuples	- stores all the tuples in tree into file
 */
void
dumptuples(FILE *file, Sort *node)
{
    register struct	leftist	*tp;
    register struct	leftist	*newp;
    struct leftist **treep = &PS(node)->Tuples;
    LeftistContext context = &PS(node)->treeContext;
    HeapTuple	tup;
    int memtupindex = 0;

    if (! PS(node)->using_tape_files && PS(node)->tupcount) {
	Assert(PS(node)->memtuples == NULL);
    	PS(node)->memtuples = palloc(PS(node)->tupcount * sizeof(HeapTuple));
    }
    
    tp = *treep;
    while (tp != NULL) {
	tup = tp->lt_tuple;
	if (tp->lt_dist == 1)			/* lt_right == NULL */
	    newp = tp->lt_left;
	else
	    newp = lmerge(tp->lt_left, tp->lt_right, context);
	FREEMEM(node,sizeof (struct leftist));
	FREE(tp);
	if (PS(node)->using_tape_files) {
	    PUTTUP(node, tup, file);
	    FREEMEM(node,tup->t_len);
	    FREE(tup);
	}
	else
	    PS(node)->memtuples[memtupindex++] = tup;

	tp = newp;
    }
    *treep = NULL;
}

/*
 *      psort_grabtuple - gets a tuple from the sorted file and returns it.
 *			  If there are no tuples left, returns NULL.
 *			  Should not call psort_end unless this has returned
 *		          a NULL indicating the last tuple has been processed.
 */
HeapTuple
psort_grabtuple(Sort *node)
{
    register HeapTuple  tup;
    long		tuplen;

    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    if (PS(node)->using_tape_files == true) {
	if (!feof(PS(node)->psort_grab_file)) {
	    if (GETLEN(tuplen, PS(node)->psort_grab_file) && tuplen != 0) {
	    	tup = (HeapTuple)palloc((unsigned)tuplen);
		SETTUPLEN(tup, tuplen);
	    	GETTUP(node, tup, tuplen, PS(node)->psort_grab_file);

	    	/* Update current merged sort file position */
		PS(node)->psort_current += tuplen;

	    	return tup;
	    }
	    else
	    	return NULL;
    	}
    	else
	    return NULL;
    }
    else {
    	if (PS(node)->psort_current < PS(node)->tupcount)
	    return PS(node)->memtuples[PS(node)->psort_current++];
    	else
	    return NULL;
    }
}

/*
 *      psort_markpos   - saves current position in the merged sort file
 */
void
psort_markpos(Sort *node)
{
    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    PS(node)->psort_saved = PS(node)->psort_current;
}

/*
 *      psort_restorepos- restores current position in merged sort file to
 *			  last saved position
 */
void
psort_restorepos(Sort *node)
{
    Assert(node != (Sort *) NULL);
    Assert(PS(node) != (Psortstate *) NULL);

    if (PS(node)->using_tape_files == true)
	fseek(PS(node)->psort_grab_file, PS(node)->psort_saved, SEEK_SET);
    PS(node)->psort_current = PS(node)->psort_saved;
}

/*
 *      psort_end       - unlinks the tape files, and cleans up. Should not be
 *			  called unless psort_grabtuple has returned a NULL.
 */
void
psort_end(Sort *node)
{
    register struct     tape    *tp;

    if (!node->cleaned) {
	Assert(node != (Sort *) NULL);
/* 	Assert(PS(node) != (Psortstate *) NULL); */

	/*
	 * I'm changing this because if we are sorting a relation
	 * with no tuples, psortstate is NULL.
	 */
	if (PS(node) != (Psortstate *) NULL) {
	    if (PS(node)->using_tape_files == true)
		for (tp = PS(node)->Tape + PS(node)->TapeRange; tp >= PS(node)->Tape; tp--)
		    destroytape(tp->tp_file);
	    else if (PS(node)->memtuples)
		pfree(PS(node)->memtuples);
	    
	    NDirectFileRead +=
		(int)ceil((double)PS(node)->BytesRead / BLCKSZ);
	    NDirectFileWrite +=
		(int)ceil((double)PS(node)->BytesWritten / BLCKSZ);

	    pfree((void *)node->psortstate);

	    node->cleaned = TRUE;
	}
    }
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
    register 	struct		tapelst	*tp;
    FILE			*file;
    static	int		tapeinit = 0;
    char			*mktemp();
    static	unsigned int	uniqueFileId = 0;
    extern	int		errno;
    char			uniqueName[MAXPGPATH];
    
    tp = (struct tapelst *)palloc((unsigned)sizeof (struct tapelst));
    
    sprintf(uniqueName, "%spg_psort.%d.%d", TEMPDIR, (int)getpid(), uniqueFileId);
    uniqueFileId++;
    
    tapeinit = 1;
    
    tp->tl_name = palloc((unsigned)sizeof(uniqueName));
    
    /*
     * now, copy template with final null into palloc'd space
     */
    
    memmove(tp->tl_name, uniqueName, strlen(uniqueName));
    
    
    file = AllocateFile(tp->tl_name, "w+");
    if (file == NULL)
	elog(WARN,"Open: %s in %s line %d, %s", tp->tl_name,
            __FILE__, __LINE__, strerror(errno));
    
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
	FreeFile(file);
	unlink(tp->tl_name);
	FREE(tp->tl_name);
	FREE(tp);
    } else
	for ( ; ; ) {
	    if (tp->tl_next == NULL)
		elog(FATAL, "destroytape: tape not found");
	    if (tp->tl_next->tl_fd == fd) {
		FreeFile(file);
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
