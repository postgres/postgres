/*-------------------------------------------------------------------------
 *
 * psort.c--
 *	  Polyphase merge sort.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/psort.c,v 1.38 1998/02/23 06:27:39 vadim Exp $
 *
 * NOTES
 *		Sorts the first relation into the second relation.
 *
 *		The old psort.c's routines formed a temporary relation from the merged
 * sort files. This version keeps the files around instead of generating the
 * relation from them, and provides interface functions to the file so that
 * you can grab tuples, mark a position in the file, restore a position in the
 * file. You must now explicitly call an interface function to end the sort,
 * psort_end, when you are done.
 *		Now most of the global variables are stuck in the Sort nodes, and
 * accessed from there (they are passed to all the psort routines) so that
 * each sort running has its own separate state. This is facilitated by having
 * the Sort nodes passed in to all the interface functions.
 *		The one global variable that all the sorts still share is SortMemory.
 *		You should now be allowed to run two or more psorts concurrently,
 * so long as the memory they eat up is not greater than SORTMEM, the initial
 * value of SortMemory.											-Rex 2.15.1995
 *
 *	  Use the tape-splitting method (Knuth, Vol. III, pp281-86) in the future.
 *
 *		Arguments? Variables?
 *				MAXMERGE, MAXTAPES
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "postgres.h"
#include "miscadmin.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "storage/buf.h"
#include "storage/fd.h"
#include "utils/lselect.h"
#include "utils/portal.h"		/* for {Start,End}PortalAllocMode */
#include "utils/psort.h"
#include "utils/rel.h"

static bool createfirstrun(Sort * node);
static bool createrun(Sort * node, FILE * file);
static void destroytape(FILE * file);
static void dumptuples(FILE * file, Sort * node);
static FILE *gettape(void);
static void initialrun(Sort * node);
static void inittapes(Sort * node);
static void merge(Sort * node, struct tape * dest);
static FILE *mergeruns(Sort * node);
static HeapTuple tuplecopy(HeapTuple tup);
static int _psort_cmp (HeapTuple *ltup, HeapTuple *rtup);



#define TEMPDIR "./"

/* 
 * tlenzero used to delimit runs; both vars below must have
 * the same size as HeapTuple->t_len
 */
static unsigned int tlenzero = 0;
static unsigned int tlendummy;

static TupleDesc	PsortTupDesc;
static ScanKey		PsortKeys;		/* used by _psort_cmp */
static int			PsortNkeys;

/*
 * old psort global variables
 *
 * (These are the global variables from the old psort. They are still used,
 *	but are now accessed from Sort nodes using the PS macro. Note that while
 *	these variables will be accessed by PS(node)->whatever, they will still
 *	be called by their original names within the comments!		-Rex 2.10.1995)
 *
 * LeftistContextData	treeContext;
 *
 * static		int		TapeRange;				number of tapes - 1 (T)
 * static		int		Level;					(l)
 * static		int		TotalDummy;				summation of tp_dummy
 * static struct tape	*Tape;
 *
 * static		int		BytesRead;				to keep track of # of IO
 * static		int		BytesWritten;
 *
 * struct leftist		*Tuples;				current tuples in memory
 *
 * FILE					*psort_grab_file;		this holds tuples grabbed
 *												   from merged sort runs
 * long					psort_current;			current file position
 * long					psort_saved;			file position saved for
 *												   mark and restore
 */

/*
 * PS - Macro to access and cast psortstate from a Sort node
 */
#define PS(N) ((Psortstate *)N->psortstate)

/*
 *		psort_begin		- polyphase merge sort entry point. Sorts the subplan
 *						  into a temporary file psort_grab_file. After
 *						  this is called, calling the interface function
 *						  psort_grabtuple iteratively will get you the sorted
 *						  tuples. psort_end then finishes the sort off, after
 *						  all the tuples have been grabbed.
 *
 *						  Allocates and initializes sort node's psort state.
 */
bool
psort_begin(Sort * node, int nkeys, ScanKey key)
{

	node->psortstate = (struct Psortstate *) palloc(sizeof(struct Psortstate));

	AssertArg(nkeys >= 1);
	AssertArg(key[0].sk_attno != 0);
	AssertArg(key[0].sk_procedure != 0);

	PS(node)->BytesRead = 0;
	PS(node)->BytesWritten = 0;
	PS(node)->treeContext.tupDesc =
		ExecGetTupType(outerPlan((Plan *) node));
	PS(node)->treeContext.nKeys = nkeys;
	PS(node)->treeContext.scanKeys = key;
	PS(node)->treeContext.sortMem = SortMem * 1024;

	PS(node)->Tuples = NULL;
	PS(node)->tupcount = 0;

	PS(node)->using_tape_files = false;
	PS(node)->all_fetched = false;
	PS(node)->psort_grab_file = NULL;
	PS(node)->memtuples = NULL;

	initialrun(node);

	if (PS(node)->tupcount == 0)
		return false;

	if (PS(node)->using_tape_files && PS(node)->psort_grab_file == NULL)
		PS(node)->psort_grab_file = mergeruns(node);

	PS(node)->psort_current = 0;
	PS(node)->psort_saved = 0;

	return true;
}

/*
 *		inittapes		- initializes the tapes
 *						- (polyphase merge Alg.D(D1)--Knuth, Vol.3, p.270)
 *		Returns:
 *				number of allocated tapes
 */
static void
inittapes(Sort * node)
{
	int i;
	struct tape *tp;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	/*
	 * ASSERT(ntapes >= 3 && ntapes <= MAXTAPES, "inittapes: Invalid
	 * number of tapes to initialize.\n");
	 */

	tp = PS(node)->Tape;
	for (i = 0; i < MAXTAPES && (tp->tp_file = gettape()) != NULL; i++)
	{
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
		elog(ERROR, "inittapes: Could only allocate %d < 3 tapes\n",
			 PS(node)->TapeRange + 1);

	PS(node)->Level = 1;
	PS(node)->TotalDummy = PS(node)->TapeRange;

	PS(node)->using_tape_files = true;
}

/*
 *		PUTTUP			- writes the next tuple
 *		ENDRUN			- mark end of run
 *		GETLEN			- reads the length of the next tuple
 *		ALLOCTUP		- returns space for the new tuple
 *		SETTUPLEN		- stores the length into the tuple
 *		GETTUP			- reads the tuple
 *
 *		Note:
 *				LEN field must be as HeapTuple->t_len; FP is a stream
 */


#define PUTTUP(NODE, TUP, FP) do {\
	((Psortstate *)NODE->psortstate)->BytesWritten += (TUP)->t_len; \
	fwrite((char *)TUP, (TUP)->t_len, 1, FP); \
	fwrite((char *)&((TUP)->t_len), sizeof (tlendummy), 1, FP); \
	} while (0)
#define ENDRUN(FP)		fwrite((char *)&tlenzero, sizeof (tlenzero), 1, FP)
#define GETLEN(LEN, FP) fread((char *)&(LEN), sizeof (tlenzero), 1, FP)
#define ALLOCTUP(LEN)	((HeapTuple)palloc((unsigned)LEN))
#define GETTUP(NODE, TUP, LEN, FP) do {\
	IncrProcessed(); \
	((Psortstate *)NODE->psortstate)->BytesRead += (LEN) - sizeof (tlenzero); \
	fread((char *)(TUP) + sizeof (tlenzero), (LEN) - sizeof (tlenzero), 1, FP); \
	fread((char *)&tlendummy, sizeof (tlendummy), 1, FP); \
	} while (0)
#define SETTUPLEN(TUP, LEN)		(TUP)->t_len = LEN

 /*
  * USEMEM			- record use of memory FREEMEM		   - record
  * freeing of memory FULLMEM		  - 1 iff a tuple will fit
  */

#define USEMEM(NODE,AMT)		PS(node)->treeContext.sortMem -= (AMT)
#define FREEMEM(NODE,AMT)		PS(node)->treeContext.sortMem += (AMT)
#define LACKMEM(NODE)			(PS(node)->treeContext.sortMem <= BLCKSZ)	/* not accurate */
#define TRACEMEM(FUNC)
#define TRACEOUT(FUNC, TUP)

/*
 *		initialrun		- distributes tuples from the relation
 *						- (replacement selection(R2-R3)--Knuth, Vol.3, p.257)
 *						- (polyphase merge Alg.D(D2-D4)--Knuth, Vol.3, p.271)
 *
 *		Explaination:
 *				Tuples are distributed to the tapes as in Algorithm D.
 *				A "tuple" with t_size == 0 is used to mark the end of a run.
 *
 *		Note:
 *				The replacement selection algorithm has been modified
 *				to go from R1 directly to R3 skipping R2 the first time.
 *
 *				Maybe should use closer(rdesc) before return
 *				Perhaps should adjust the number of tapes if less than n.
 *				used--v. likely to have problems in mergeruns().
 *				Must know if should open/close files before each
 *				call to  psort()?	If should--messy??
 *
 *		Possible optimization:
 *				put the first xxx runs in quickly--problem here since
 *				I (perhaps prematurely) combined the 2 algorithms.
 *				Also, perhaps allocate tapes when needed. Split into 2 funcs.
 */
static void
initialrun(Sort * node)
{
	/* struct tuple   *tup; */
	struct tape *tp;
	int			baseruns;		/* D:(a) */
	int			extrapasses;	/* EOF */

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	tp = PS(node)->Tape;

	if (createfirstrun(node))
	{
		Assert (PS(node)->using_tape_files);
		extrapasses = 0;
	}
	else			/* all tuples fetched */
	{
		if ( !PS(node)->using_tape_files )	/* empty or sorted in memory */
			return;
		/* 
		 * if PS(node)->Tuples == NULL then we have single (sorted) run
		 * which can be used as result grab file! So, we may avoid 
		 * mergeruns - it will just copy this run to new file.
		 */
		if ( PS(node)->Tuples == NULL )
		{
			PS(node)->psort_grab_file = PS(node)->Tape->tp_file;
			rewind (PS(node)->psort_grab_file);
			return;
		}
		extrapasses = 2;
	}

	for (;;)
	{
		tp->tp_dummy--;
		PS(node)->TotalDummy--;
		if (tp->tp_dummy < (tp + 1)->tp_dummy)
			tp++;
		else if (tp->tp_dummy != 0)
			tp = PS(node)->Tape;
		else
		{
			PS(node)->Level++;
			baseruns = PS(node)->Tape[0].tp_fib;
			for (tp = PS(node)->Tape;
				 tp - PS(node)->Tape < PS(node)->TapeRange; tp++)
			{
				PS(node)->TotalDummy +=
					(tp->tp_dummy = baseruns
					 + (tp + 1)->tp_fib
					 - tp->tp_fib);
				tp->tp_fib = baseruns
					+ (tp + 1)->tp_fib;
			}
			tp = PS(node)->Tape;/* D4 */
		}						/* D3 */
		if (extrapasses)
			if (--extrapasses)
			{
				dumptuples(tp->tp_file, node);
				ENDRUN(tp->tp_file);
				continue;
			}
			else
				break;

		if ((bool) createrun(node, tp->tp_file) == false)
			extrapasses = 1 + (PS(node)->Tuples != NULL);
		/* D2 */
	}
	for (tp = PS(node)->Tape + PS(node)->TapeRange; tp >= PS(node)->Tape; tp--)
		rewind(tp->tp_file);	/* D. */
}

/*
 *		createfirstrun		- tries to sort tuples in memory using qsort 
 *						until LACKMEM; if not enough memory then switches
 *						to tape method
 *
 *		Returns:
 *				FALSE iff process through end of relation
 *				Tuples contains the tuples for the following run upon exit
 */
static bool
createfirstrun(Sort *node)
{
    HeapTuple			tup;
	bool				foundeor = false;
	HeapTuple		   *memtuples;
	int					t_last = -1;
	int					t_free = 1000;
	TupleTableSlot	   *cr_slot;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(!PS(node)->using_tape_files);
	Assert(PS(node)->memtuples == NULL);
	Assert(PS(node)->tupcount == 0);
	if (LACKMEM(node))
		elog (FATAL, "psort: LACKMEM in createfirstrun");
	
	memtuples = palloc(t_free * sizeof(HeapTuple));
	
	for (;;)
	{
		if ( LACKMEM (node) )
			break;
		
		/*
		 * About to call ExecProcNode, it can mess up the state if it
		 * eventually calls another Sort node. So must stow it away here
		 * for the meantime.										-Rex
		 * 2.2.1995
		 */

		cr_slot = ExecProcNode(outerPlan((Plan *) node), (Plan *) node);

		if (TupIsNull(cr_slot))
		{
			foundeor = true;
			break;
		}
		
		tup = tuplecopy(cr_slot->val);
		ExecClearTuple(cr_slot);

		IncrProcessed();
		USEMEM(node, tup->t_len);
		TRACEMEM(createfirstrun);
		if ( t_free <= 0 )
		{
		    t_free = 1000;
			memtuples = repalloc (memtuples, 
	    				(t_last + t_free + 1) * sizeof (HeapTuple));
		}
		t_last++;
		t_free--;
		memtuples[t_last] = tup;
	}
	
	if ( t_last < 0 )			/* empty */
	{
		Assert (foundeor);
		pfree (memtuples);
		return (false);
	}
	t_last++;
	PS(node)->tupcount = t_last;
	PsortTupDesc = PS(node)->treeContext.tupDesc;
	PsortKeys = PS(node)->treeContext.scanKeys;
	PsortNkeys = PS(node)->treeContext.nKeys;
    qsort (memtuples, t_last, sizeof (HeapTuple), 
    	(int (*)(const void *,const void *))_psort_cmp);
	
	if ( LACKMEM (node) )	/* in-memory sort is impossible */
	{
    	int t;
		
		Assert (!foundeor);
		inittapes(node);
		/* put tuples into leftist tree for createrun */
		for (t = t_last - 1 ; t >= 0; t--)
			puttuple(&PS(node)->Tuples, memtuples[t], 0, &PS(node)->treeContext);
		pfree (memtuples);
		foundeor = !createrun (node, PS(node)->Tape->tp_file);
	}
	else
	{
		Assert (foundeor);
		PS(node)->memtuples = memtuples;
	}
		
	return (!foundeor);
}

/*
 *		createrun		- places the next run on file, grabbing the tuples by
 *						executing the subplan passed in
 *
 *		Uses:
 *				Tuples, which should contain any tuples for this run
 *
 *		Returns:
 *				FALSE iff process through end of relation
 *				Tuples contains the tuples for the following run upon exit
 */
static bool
createrun(Sort * node, FILE * file)
{
	HeapTuple	lasttuple;
	HeapTuple	tup;
	TupleTableSlot	   *cr_slot;
	HeapTuple		   *memtuples;
	int					t_last = -1;
	int					t_free = 1000;
	bool				foundeor = false;
	short				junk;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert (PS(node)->using_tape_files);

	lasttuple = NULL;
	memtuples = palloc(t_free * sizeof(HeapTuple));
	
	for (;;)
	{
		while (LACKMEM(node) && PS(node)->Tuples != NULL)
		{
			if (lasttuple != NULL)
			{
				FREEMEM(node, lasttuple->t_len);
				FREE(lasttuple);
				TRACEMEM(createrun);
			}
			lasttuple = gettuple(&PS(node)->Tuples, &junk,
										   &PS(node)->treeContext);
			PUTTUP(node, lasttuple, file);
			TRACEOUT(createrun, lasttuple);
		}
		
		if (LACKMEM(node))
			break;
		
		/*
		 * About to call ExecProcNode, it can mess up the state if it
		 * eventually calls another Sort node. So must stow it away here
		 * for the meantime.										-Rex
		 * 2.2.1995
		 */

		cr_slot = ExecProcNode(outerPlan((Plan *) node), (Plan *) node);

		if (TupIsNull(cr_slot))
		{
			foundeor = true;
			break;
		}
		else
		{
			tup = tuplecopy(cr_slot->val);
			ExecClearTuple(cr_slot);
			PS(node)->tupcount++;
		}

		IncrProcessed();
		USEMEM(node, tup->t_len);
		TRACEMEM(createrun);
		if (lasttuple != NULL && tuplecmp(tup, lasttuple,
										  &PS(node)->treeContext))
		{
			if ( t_free <= 0 )
			{
			    t_free = 1000;
				memtuples = repalloc (memtuples, 
		    				(t_last + t_free + 1) * sizeof (HeapTuple));
			}
			t_last++;
			t_free--;
			memtuples[t_last] = tup;
		}
		else
			puttuple(&PS(node)->Tuples, tup, 0, &PS(node)->treeContext);
	}
	if (lasttuple != NULL)
	{
		FREEMEM(node, lasttuple->t_len);
		FREE(lasttuple);
		TRACEMEM(createrun);
	}
	dumptuples(file, node);
	ENDRUN(file);				/* delimit the end of the run */
	
	t_last++;
	/* put tuples for the next run into leftist tree */
	if ( t_last >= 1 )
	{
		int t;
		
		PsortTupDesc = PS(node)->treeContext.tupDesc;
		PsortKeys = PS(node)->treeContext.scanKeys;
		PsortNkeys = PS(node)->treeContext.nKeys;
    	qsort (memtuples, t_last, sizeof (HeapTuple), 
    		(int (*)(const void *,const void *))_psort_cmp);
		for (t = t_last - 1 ; t >= 0; t--)
			puttuple(&PS(node)->Tuples, memtuples[t], 0, &PS(node)->treeContext);
	}
	
	pfree (memtuples);

	return (!foundeor);
}

/*
 *		tuplecopy		- see also tuple.c:palloctup()
 *
 *		This should eventually go there under that name?  And this will
 *		then use palloc directly (see version -r1.2).
 */
static HeapTuple
tuplecopy(HeapTuple tup)
{
	HeapTuple	rettup;

	if (!HeapTupleIsValid(tup))
	{
		return (NULL);			/* just in case */
	}
	rettup = (HeapTuple) palloc(tup->t_len);
	memmove((char *) rettup, (char *) tup, tup->t_len); /* XXX */
	return (rettup);
}

/*
 *		mergeruns		- merges all runs from input tapes
 *						  (polyphase merge Alg.D(D6)--Knuth, Vol.3, p271)
 *
 *		Returns:
 *				file of tuples in order
 */
static FILE *
mergeruns(Sort * node)
{
	struct tape *tp;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(PS(node)->using_tape_files == true);

	tp = PS(node)->Tape + PS(node)->TapeRange;
	merge(node, tp);
	rewind(tp->tp_file);
	while (--PS(node)->Level != 0)
	{
		tp = tp->tp_prev;
		rewind(tp->tp_file);
		/* resettape(tp->tp_file);	  -not sufficient */
		merge(node, tp);
		rewind(tp->tp_file);
	}
	return (tp->tp_file);
}

/*
 *		merge			- handles a single merge of the tape
 *						  (polyphase merge Alg.D(D5)--Knuth, Vol.3, p271)
 */
static void
merge(Sort * node, struct tape * dest)
{
	HeapTuple tup;
	struct tape *lasttp;		/* (TAPE[P]) */
	struct tape *tp;
	struct leftist *tuples;
	FILE		   *destfile;
	int				times;			/* runs left to merge */
	int				outdummy;		/* complete dummy runs */
	short			fromtape;
	unsigned int	tuplen;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(PS(node)->using_tape_files == true);

	lasttp = dest->tp_prev;
	times = lasttp->tp_fib;
	for (tp = lasttp; tp != dest; tp = tp->tp_prev)
		tp->tp_fib -= times;
	tp->tp_fib += times;
	/* Tape[].tp_fib (A[]) is set to proper exit values */

	if (PS(node)->TotalDummy < PS(node)->TapeRange)		/* no complete dummy
														 * runs */
		outdummy = 0;
	else
	{
		outdummy = PS(node)->TotalDummy;		/* a large positive number */
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
	while (times-- != 0)
	{							/* merge one run */
		tuples = NULL;
		if (PS(node)->TotalDummy == 0)
			for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev)
			{
				GETLEN(tuplen, tp->tp_file);
				tup = ALLOCTUP(tuplen);
				USEMEM(node, tuplen);
				TRACEMEM(merge);
				SETTUPLEN(tup, tuplen);
				GETTUP(node, tup, tuplen, tp->tp_file);
				puttuple(&tuples, tup, tp - PS(node)->Tape,
						 &PS(node)->treeContext);
			}
		else
		{
			for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev)
			{
				if (tp->tp_dummy != 0)
				{
					tp->tp_dummy--;
					PS(node)->TotalDummy--;
				}
				else
				{
					GETLEN(tuplen, tp->tp_file);
					tup = ALLOCTUP(tuplen);
					USEMEM(node, tuplen);
					TRACEMEM(merge);
					SETTUPLEN(tup, tuplen);
					GETTUP(node, tup, tuplen, tp->tp_file);
					puttuple(&tuples, tup, tp - PS(node)->Tape,
							 &PS(node)->treeContext);
				}
			}
		}
		while (tuples != NULL)
		{
			/* possible optimization by using count in tuples */
			tup = gettuple(&tuples, &fromtape, &PS(node)->treeContext);
			PUTTUP(node, tup, destfile);
			FREEMEM(node, tup->t_len);
			FREE(tup);
			TRACEMEM(merge);
			GETLEN(tuplen, PS(node)->Tape[fromtape].tp_file);
			if (tuplen == 0)
				;
			else
			{
				tup = ALLOCTUP(tuplen);
				USEMEM(node, tuplen);
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
static void
dumptuples(FILE * file, Sort * node)
{
	struct leftist *tp;
	struct leftist *newp;
	struct leftist **treep = &PS(node)->Tuples;
	LeftistContext context = &PS(node)->treeContext;
	HeapTuple	tup;

	Assert (PS(node)->using_tape_files);

	tp = *treep;
	while (tp != NULL)
	{
		tup = tp->lt_tuple;
		if (tp->lt_dist == 1)	/* lt_right == NULL */
			newp = tp->lt_left;
		else
			newp = lmerge(tp->lt_left, tp->lt_right, context);
		pfree(tp);
		PUTTUP(node, tup, file);
		FREEMEM(node, tup->t_len);
		FREE(tup);

		tp = newp;
	}
	*treep = NULL;
}

/*
 *		psort_grabtuple - gets a tuple from the sorted file and returns it.
 *						  If there are no tuples left, returns NULL.
 *						  Should not call psort_end unless this has returned
 *						  a NULL indicating the last tuple has been processed.
 */
HeapTuple
psort_grabtuple(Sort * node, bool * should_free)
{
	HeapTuple	tup;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	if (PS(node)->using_tape_files == true)
	{
		unsigned int	tuplen;
		
		*should_free = true;
		if (ScanDirectionIsForward (node->plan.state->es_direction))
		{
			if (PS(node)->all_fetched)
				return NULL;
			if (GETLEN(tuplen, PS(node)->psort_grab_file) && tuplen != 0)
			{
				tup = (HeapTuple) palloc((unsigned) tuplen);
				SETTUPLEN(tup, tuplen);
				GETTUP(node, tup, tuplen, PS(node)->psort_grab_file);

				/* Update current merged sort file position */
				PS(node)->psort_current += tuplen + sizeof (tlendummy);
				return tup;
			}
			else
			{
				PS(node)->all_fetched = true;
				return NULL;
			}
		}
		/* Backward */
		if (PS(node)->psort_current <= sizeof (tlendummy))
			return NULL;
		/* 
		 * if all tuples are fetched already then we return last tuple, 
		 * else - tuple before last returned.
		 */
		if (PS(node)->all_fetched)
		{
			/* psort_current is pointing to the zero tuplen at the end of file */
			fseek(PS(node)->psort_grab_file, 
					PS(node)->psort_current - sizeof (tlendummy), SEEK_SET);
			GETLEN(tuplen, PS(node)->psort_grab_file);
			if (PS(node)->psort_current < tuplen)
				elog (FATAL, "psort_grabtuple: too big last tuple len in backward scan");
			PS(node)->all_fetched = false;
		}
		else
		{
			/* move to position of end tlen of prev tuple */
			PS(node)->psort_current -= sizeof (tlendummy);
			fseek(PS(node)->psort_grab_file, PS(node)->psort_current, SEEK_SET);
			GETLEN(tuplen, PS(node)->psort_grab_file);	/* get tlen of prev tuple */
			if (tuplen == 0)
				elog (FATAL, "psort_grabtuple: tuplen is 0 in backward scan");
			if (PS(node)->psort_current <= tuplen + sizeof (tlendummy))
			{	/* prev tuple should be first one */
				if (PS(node)->psort_current != tuplen)
					elog (FATAL, "psort_grabtuple: first tuple expected in backward scan");
				PS(node)->psort_current = 0;
				fseek(PS(node)->psort_grab_file, PS(node)->psort_current, SEEK_SET);
				return NULL;
			}
			/* 
			 * Get position of prev tuple. This tuple becomes current tuple
			 * now and we have to return previous one.
			 */
			PS(node)->psort_current -= tuplen;
			/* move to position of end tlen of prev tuple */
			fseek(PS(node)->psort_grab_file, 
					PS(node)->psort_current - sizeof (tlendummy), SEEK_SET);
			GETLEN(tuplen, PS(node)->psort_grab_file);
			if (PS(node)->psort_current < tuplen + sizeof (tlendummy))
				elog (FATAL, "psort_grabtuple: too big tuple len in backward scan");
		}
		/* 
		 * move to prev (or last) tuple start position + sizeof(t_len) 
		 */
		fseek(PS(node)->psort_grab_file,
				PS(node)->psort_current - tuplen, SEEK_SET);
		tup = (HeapTuple) palloc((unsigned) tuplen);
		SETTUPLEN(tup, tuplen);
		GETTUP(node, tup, tuplen, PS(node)->psort_grab_file);
		return tup;		/* file position is equal to psort_current */
	}
	else
	{
		*should_free = false;
		if (ScanDirectionIsForward (node->plan.state->es_direction))
		{
			if (PS(node)->psort_current < PS(node)->tupcount)
				return (PS(node)->memtuples[PS(node)->psort_current++]);
			else
			{
				PS(node)->all_fetched = true;
				return NULL;
			}
		}
		/* Backward */
		if (PS(node)->psort_current <= 0)
			return NULL;
		/* 
		 * if all tuples are fetched already then we return last tuple, 
		 * else - tuple before last returned.
		 */
		if (PS(node)->all_fetched)		
			PS(node)->all_fetched = false;
		else
		{
			PS(node)->psort_current--;			/* last returned tuple */
			if (PS(node)->psort_current <= 0)
				return NULL;
		}
		return (PS(node)->memtuples[PS(node)->psort_current - 1]);
	}
}

/*
 *		psort_markpos	- saves current position in the merged sort file
 */
void
psort_markpos(Sort * node)
{
	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	PS(node)->psort_saved = PS(node)->psort_current;
}

/*
 *		psort_restorepos- restores current position in merged sort file to
 *						  last saved position
 */
void
psort_restorepos(Sort * node)
{
	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	if (PS(node)->using_tape_files == true)
		fseek(PS(node)->psort_grab_file, PS(node)->psort_saved, SEEK_SET);
	PS(node)->psort_current = PS(node)->psort_saved;
}

/*
 *		psort_end		- unlinks the tape files, and cleans up. Should not be
 *						  called unless psort_grabtuple has returned a NULL.
 */
void
psort_end(Sort * node)
{
	struct tape *tp;

	if (!node->cleaned)
	{
		/*
		 * I'm changing this because if we are sorting a relation with no
		 * tuples, psortstate is NULL.
		 */
		if (PS(node) != (Psortstate *) NULL)
		{
			if (PS(node)->using_tape_files == true)
				for (tp = PS(node)->Tape + PS(node)->TapeRange; tp >= PS(node)->Tape; tp--)
					destroytape(tp->tp_file);
			else if (PS(node)->memtuples)
				pfree(PS(node)->memtuples);

			NDirectFileRead +=
				(int) ceil((double) PS(node)->BytesRead / BLCKSZ);
			NDirectFileWrite +=
				(int) ceil((double) PS(node)->BytesWritten / BLCKSZ);

			pfree((void *) node->psortstate);
			node->psortstate = NULL;

			node->cleaned = TRUE;
		}
	}
}

void
psort_rescan (Sort *node)
{
	/*
	 * If subnode is to be rescanned then free our previous results
	 */
	if (((Plan*) node)->lefttree->chgParam != NULL)
	{
		psort_end (node);
		node->cleaned = false;
	}
	else if (PS(node) != (Psortstate *) NULL)
	{
		PS(node)->all_fetched = false;
		PS(node)->psort_current = 0;
		PS(node)->psort_saved = 0;
		if (PS(node)->using_tape_files == true)
			rewind (PS(node)->psort_grab_file);
	}

}

/*
 *		gettape			- handles access temporary files in polyphase merging
 *
 *		Optimizations:
 *				If guarenteed that only one sort running/process,
 *				can simplify the file generation--and need not store the
 *				name for later unlink.
 */

struct tapelst
{
	char	   *tl_name;
	int			tl_fd;
	struct tapelst *tl_next;
};

static struct tapelst *Tapes = NULL;

/*
 *		gettape			- returns an open stream for writing/reading
 *
 *		Returns:
 *				Open stream for writing/reading.
 *				NULL if unable to open temporary file.
 */
static FILE *
gettape()
{
	struct tapelst *tp;
	FILE	   *file;
	static int	tapeinit = 0;
	char	   *mktemp();
	static unsigned int uniqueFileId = 0;
	extern int	errno;
	char		uniqueName[MAXPGPATH];

	tp = (struct tapelst *) palloc((unsigned) sizeof(struct tapelst));

	sprintf(uniqueName, "%spg_psort.%d.%d", TEMPDIR, (int) MyProcPid, uniqueFileId);
	uniqueFileId++;

	tapeinit = 1;

	tp->tl_name = palloc((unsigned) sizeof(uniqueName));

	/*
	 * now, copy template with final null into palloc'd space
	 */

	StrNCpy(tp->tl_name, uniqueName, MAXPGPATH);


	file = AllocateFile(tp->tl_name, "w+");
	if (file == NULL)
		elog(ERROR, "Open: %s in %s line %d, %s", tp->tl_name,
			 __FILE__, __LINE__, strerror(errno));

	tp->tl_fd = fileno(file);
	tp->tl_next = Tapes;
	Tapes = tp;
	return (file);
}

/*
 *		resettape		- resets the tape to size 0
 */
#ifdef NOT_USED
static void
resettape(FILE * file)
{
	struct tapelst *tp;
	int fd;

	Assert(PointerIsValid(file));

	fd = fileno(file);
	for (tp = Tapes; tp != NULL && tp->tl_fd != fd; tp = tp->tl_next)
		;
	if (tp == NULL)
		elog(ERROR, "resettape: tape not found");

	file = freopen(tp->tl_name, "w+", file);
	if (file == NULL)
	{
		elog(FATAL, "could not freopen temporary file");
	}
}

#endif

/*
 *		distroytape		- unlinks the tape
 *
 *		Efficiency note:
 *				More efficient to destroy more recently allocated tapes first.
 *
 *		Possible bugs:
 *				Exits instead of returning status, if given invalid tape.
 */
static void
destroytape(FILE * file)
{
	struct tapelst *tp,
			   *tq;
	int fd;

	if ((tp = Tapes) == NULL)
		elog(FATAL, "destroytape: tape not found");

	if ((fd = fileno(file)) == tp->tl_fd)
	{
		Tapes = tp->tl_next;
		FreeFile(file);
		unlink(tp->tl_name);
		FREE(tp->tl_name);
		FREE(tp);
	}
	else
		for (;;)
		{
			if (tp->tl_next == NULL)
				elog(FATAL, "destroytape: tape not found");
			if (tp->tl_next->tl_fd == fd)
			{
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

static int
_psort_cmp (HeapTuple *ltup, HeapTuple *rtup)
{
    Datum	lattr, rattr;
    int		nkey;
    int		result = 0;
    bool	isnull1, isnull2;
    
    for (nkey = 0; nkey < PsortNkeys && !result; nkey++ )
    {
		lattr = heap_getattr(*ltup,
				     PsortKeys[nkey].sk_attno, 
				     PsortTupDesc,
			    	 &isnull1);
		rattr = heap_getattr(*rtup,
				     PsortKeys[nkey].sk_attno, 
				     PsortTupDesc,
				     &isnull2);
		if ( isnull1 )
		{
			if ( !isnull2 )
				result = 1;
		}
		else if ( isnull2 )
		    result = -1;
		
		else if (PsortKeys[nkey].sk_flags & SK_COMMUTE)
		{
	    	if (!(result = -(long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (rattr, lattr)))
			result = (long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (lattr, rattr);
		}
		else if (!(result = -(long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (lattr, rattr)))
		    result = (long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (rattr, lattr);
    }
    return (result);
}
