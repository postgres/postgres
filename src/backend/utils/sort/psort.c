/*-------------------------------------------------------------------------
 *
 * psort.c
 *	  Polyphase merge sort.
 *
 * See Knuth, volume 3, for more than you want to know about this algorithm.
 *
 * NOTES
 *
 * This needs to be generalized to handle index tuples as well as heap tuples,
 * so that the near-duplicate code in nbtsort.c can be eliminated.  Also,
 * I think it's got memory leak problems.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/Attic/psort.c,v 1.58 1999/10/16 19:49:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <math.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/relscan.h"
#include "executor/execdebug.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/logtape.h"
#include "utils/lselect.h"
#include "utils/psort.h"

#define MAXTAPES		7		/* See Knuth Fig. 70, p273 */

struct tape
{
	int			tp_dummy;		/* (D) */
	int			tp_fib;			/* (A) */
	int			tp_tapenum;		/* (TAPE) */
	struct tape *tp_prev;
};

/*
 * Private state of a Psort operation.  The "psortstate" field in a Sort node
 * points to one of these.  This replaces a lot of global variables that used
 * to be here...
 */
typedef struct Psortstate
{
	LeftistContextData treeContext;

	int			TapeRange;		/* number of tapes less 1 (T) */
	int			Level;			/* Knuth's l */
	int			TotalDummy;		/* sum of tp_dummy across all tapes */
	struct tape Tape[MAXTAPES];

	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

	int			BytesRead;		/* I/O statistics (useless) */
	int			BytesWritten;
	int			tupcount;

	struct leftist *Tuples;		/* current tuple tree */

	int			psort_grab_tape; /* tape number of finished output data */
	long		psort_current;	/* array index (only used if not tape) */
	/* psort_saved(_offset) holds marked position for mark and restore */
	long		psort_saved;	/* could be tape block#, or array index */
	int			psort_saved_offset;	/* lower bits of psort_saved, if tape */
	bool		using_tape_files;
	bool		all_fetched;	/* this is for cursors */

	HeapTuple  *memtuples;
} Psortstate;

/*
 * PS - Macro to access and cast psortstate from a Sort node
 */
#define PS(N) ((Psortstate *)(N)->psortstate)

static bool createfirstrun(Sort *node);
static bool createrun(Sort *node, int desttapenum);
static void dumptuples(Sort *node, int desttapenum);
static void initialrun(Sort *node);
static void inittapes(Sort *node);
static void merge(Sort *node, struct tape * dest);
static int mergeruns(Sort *node);
static int	_psort_cmp(HeapTuple *ltup, HeapTuple *rtup);

/* these are used by _psort_cmp, and are set just before calling qsort() */
static TupleDesc PsortTupDesc;
static ScanKey PsortKeys;
static int	PsortNkeys;

/*
 * tlenzero is used to write a zero to delimit runs, tlendummy is used
 * to read in length words that we don't care about.
 *
 * both vars must have the same size as HeapTuple->t_len
 */
static unsigned int tlenzero = 0;
static unsigned int tlendummy;


/*
 *		psort_begin
 *
 * polyphase merge sort entry point. Sorts the subplan
 * into memory or a temporary file. After
 * this is called, calling the interface function
 * psort_grabtuple iteratively will get you the sorted
 * tuples. psort_end releases storage when done.
 *
 * Allocates and initializes sort node's psort state.
 */
bool
psort_begin(Sort *node, int nkeys, ScanKey key)
{
	AssertArg(nkeys >= 1);
	AssertArg(key[0].sk_attno != 0);
	AssertArg(key[0].sk_procedure != 0);

	node->psortstate = (void *) palloc(sizeof(struct Psortstate));

	PS(node)->treeContext.tupDesc = ExecGetTupType(outerPlan((Plan *) node));
	PS(node)->treeContext.nKeys = nkeys;
	PS(node)->treeContext.scanKeys = key;
	PS(node)->treeContext.sortMem = SortMem * 1024;

	PS(node)->tapeset = NULL;

	PS(node)->BytesRead = 0;
	PS(node)->BytesWritten = 0;
	PS(node)->tupcount = 0;

	PS(node)->Tuples = NULL;

	PS(node)->using_tape_files = false;
	PS(node)->all_fetched = false;
	PS(node)->psort_grab_tape = -1;

	PS(node)->memtuples = NULL;

	initialrun(node);

	if (PS(node)->tupcount == 0)
		return false;

	if (PS(node)->using_tape_files && PS(node)->psort_grab_tape == -1)
		PS(node)->psort_grab_tape = mergeruns(node);

	PS(node)->psort_current = 0L;
	PS(node)->psort_saved = 0L;
	PS(node)->psort_saved_offset = 0;

	return true;
}

/*
 *		inittapes		- initializes the tapes
 *						- (polyphase merge Alg.D(D1)--Knuth, Vol.3, p.270)
 *
 * This is called only if we have found we don't have room to sort in memory.
 */
static void
inittapes(Sort *node)
{
	int			i;
	struct tape *tp;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	PS(node)->tapeset = LogicalTapeSetCreate(MAXTAPES);

	tp = PS(node)->Tape;
	for (i = 0; i < MAXTAPES; i++)
	{
		tp->tp_dummy = 1;
		tp->tp_fib = 1;
		tp->tp_tapenum = i;
		tp->tp_prev = tp - 1;
		tp++;
	}
	PS(node)->TapeRange = --tp - PS(node)->Tape;
	tp->tp_dummy = 0;
	tp->tp_fib = 0;
	PS(node)->Tape[0].tp_prev = tp;

	PS(node)->Level = 1;
	PS(node)->TotalDummy = PS(node)->TapeRange;

	PS(node)->using_tape_files = true;
}

/*
 *		PUTTUP			- writes the next tuple
 *		ENDRUN			- mark end of run
 *		TRYGETLEN		- reads the length of the next tuple, if any
 *		GETLEN			- reads the length of the next tuple, must be one
 *		ALLOCTUP		- returns space for the new tuple
 *		GETTUP			- reads the tuple
 *
 *		Note:
 *				LEN field must be as HeapTuple->t_len; FP is a stream
 */


#define PUTTUP(NODE, TUP, TAPE) \
( \
	(TUP)->t_len += HEAPTUPLESIZE, \
	PS(NODE)->BytesWritten += (TUP)->t_len, \
	LogicalTapeWrite(PS(NODE)->tapeset, (TAPE), (void*)(TUP), (TUP)->t_len), \
	LogicalTapeWrite(PS(NODE)->tapeset, (TAPE), (void*)&((TUP)->t_len), sizeof(tlendummy)), \
	(TUP)->t_len -= HEAPTUPLESIZE \
)

#define ENDRUN(NODE, TAPE) \
	LogicalTapeWrite(PS(NODE)->tapeset, (TAPE), (void *)&tlenzero, sizeof(tlenzero))

#define TRYGETLEN(NODE, LEN, TAPE) \
	(LogicalTapeRead(PS(NODE)->tapeset, (TAPE), \
					 (void *) &(LEN), sizeof(tlenzero)) == sizeof(tlenzero) \
	 && (LEN) != 0)

#define GETLEN(NODE, LEN, TAPE) \
	do { \
		if (! TRYGETLEN(NODE, LEN, TAPE)) \
			elog(ERROR, "psort: unexpected end of data"); \
	} while(0)

static void GETTUP(Sort *node, HeapTuple tup, unsigned int len, int tape)
{
	IncrProcessed();
	PS(node)->BytesRead += len;
	if (LogicalTapeRead(PS(node)->tapeset, tape,
						((char *) tup) + sizeof(tlenzero),
						len - sizeof(tlenzero)) != len - sizeof(tlenzero))
		elog(ERROR, "psort: unexpected end of data");
	tup->t_len = len - HEAPTUPLESIZE;
	tup->t_data = (HeapTupleHeader) ((char *) tup + HEAPTUPLESIZE);
	if (LogicalTapeRead(PS(node)->tapeset, tape,
						(void *) &tlendummy,
						sizeof(tlendummy)) != sizeof(tlendummy))
		elog(ERROR, "psort: unexpected end of data");
}

#define ALLOCTUP(LEN)	((HeapTuple) palloc(LEN))
#define FREE(x)			pfree((char *) (x))

 /*
  * USEMEM			- record use of memory FREEMEM		   - record
  * freeing of memory FULLMEM		  - 1 iff a tuple will fit
  */

#define USEMEM(NODE,AMT)		PS(node)->treeContext.sortMem -= (AMT)
#define FREEMEM(NODE,AMT)		PS(node)->treeContext.sortMem += (AMT)
#define LACKMEM(NODE)			(PS(node)->treeContext.sortMem <= BLCKSZ)		/* not accurate */
#define TRACEMEM(FUNC)
#define TRACEOUT(FUNC, TUP)

/*
 *		initialrun		- distributes tuples from the relation
 *						- (replacement selection(R2-R3)--Knuth, Vol.3, p.257)
 *						- (polyphase merge Alg.D(D2-D4)--Knuth, Vol.3, p.271)
 *
 *		Explanation:
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
initialrun(Sort *node)
{
	struct tape *tp;
	int			baseruns;		/* D:(a) */
	int			extrapasses;	/* EOF */
	int			tapenum;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	tp = PS(node)->Tape;

	if (createfirstrun(node))
	{
		Assert(PS(node)->using_tape_files);
		extrapasses = 0;
	}
	else
	{
		/* all tuples fetched */
		if (!PS(node)->using_tape_files)		/* empty or sorted in
												 * memory */
			return;

		/*
		 * if PS(node)->Tuples == NULL then we have single (sorted) run
		 * which can be used as result grab file! So, we may avoid
		 * mergeruns - it will just copy this run to new file.
		 */
		if (PS(node)->Tuples == NULL)
		{
			PS(node)->psort_grab_tape = PS(node)->Tape[0].tp_tapenum;
			/* freeze and rewind the finished output tape */
			LogicalTapeFreeze(PS(node)->tapeset, PS(node)->psort_grab_tape);
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
		else
		{
			if (tp->tp_dummy != 0)
				tp = PS(node)->Tape;
			else
			{
				PS(node)->Level++;
				baseruns = PS(node)->Tape[0].tp_fib;
				for (tp = PS(node)->Tape;
					 tp - PS(node)->Tape < PS(node)->TapeRange; tp++)
				{
					PS(node)->TotalDummy += (tp->tp_dummy = baseruns
											 + (tp + 1)->tp_fib
											 - tp->tp_fib);
					tp->tp_fib = baseruns
						+ (tp + 1)->tp_fib;
				}
				tp = PS(node)->Tape;	/* D4 */
			}					/* D3 */
		}
		if (extrapasses)
		{
			if (--extrapasses)
			{
				dumptuples(node, tp->tp_tapenum);
				ENDRUN(node, tp->tp_tapenum);
				continue;
			}
			else
				break;
		}
		if (createrun(node, tp->tp_tapenum) == false)
			extrapasses = 1 + (PS(node)->Tuples != NULL);
		/* D2 */
	}
	/* End of step D2: rewind all output tapes to prepare for merging */
	for (tapenum = 0; tapenum < PS(node)->TapeRange; tapenum++)
		LogicalTapeRewind(PS(node)->tapeset, tapenum, false);
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
	HeapTuple	tup;
	bool		foundeor = false;
	HeapTuple  *memtuples;
	int			t_last = -1;
	int			t_free = 1000;
	TupleTableSlot *cr_slot;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(!PS(node)->using_tape_files);
	Assert(PS(node)->memtuples == NULL);
	Assert(PS(node)->tupcount == 0);
	if (LACKMEM(node))
		elog(ERROR, "psort: LACKMEM before createfirstrun");

	memtuples = palloc(t_free * sizeof(HeapTuple));

	for (;;)
	{
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

		tup = heap_copytuple(cr_slot->val);
		ExecClearTuple(cr_slot);

		IncrProcessed();
		USEMEM(node, tup->t_len);
		TRACEMEM(createfirstrun);
		if (t_free <= 0)
		{
			t_free = 1000;
			memtuples = repalloc(memtuples,
							  (t_last + t_free + 1) * sizeof(HeapTuple));
		}
		t_last++;
		t_free--;
		memtuples[t_last] = tup;
	}

	if (t_last < 0)				/* empty */
	{
		Assert(foundeor);
		pfree(memtuples);
		return false;
	}
	t_last++;
	PS(node)->tupcount = t_last;
	PsortTupDesc = PS(node)->treeContext.tupDesc;
	PsortKeys = PS(node)->treeContext.scanKeys;
	PsortNkeys = PS(node)->treeContext.nKeys;
	qsort(memtuples, t_last, sizeof(HeapTuple),
		  (int (*) (const void *, const void *)) _psort_cmp);

	if (LACKMEM(node))			/* in-memory sort is impossible */
	{
		int			t;

		Assert(!foundeor);
		inittapes(node);
		/* put tuples into leftist tree for createrun */
		for (t = t_last - 1; t >= 0; t--)
			puttuple(&PS(node)->Tuples, memtuples[t], 0, &PS(node)->treeContext);
		pfree(memtuples);
		foundeor = ! createrun(node, PS(node)->Tape->tp_tapenum);
	}
	else
	{
		Assert(foundeor);
		PS(node)->memtuples = memtuples;
	}

	return !foundeor;
}

/*
 *		createrun
 *
 * Create the next run and write it to desttapenum, grabbing the tuples by
 * executing the subplan passed in
 *
 *		Uses:
 *				Tuples, which should contain any tuples for this run
 *
 *		Returns:
 *				FALSE iff process through end of relation
 *				Tuples contains the tuples for the following run upon exit
 */
static bool
createrun(Sort *node, int desttapenum)
{
	HeapTuple	lasttuple;
	HeapTuple	tup;
	TupleTableSlot *cr_slot;
	HeapTuple  *memtuples;
	int			t_last = -1;
	int			t_free = 1000;
	bool		foundeor = false;
	short		junk;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(PS(node)->using_tape_files);

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
			PUTTUP(node, lasttuple, desttapenum);
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
			tup = heap_copytuple(cr_slot->val);
			ExecClearTuple(cr_slot);
			PS(node)->tupcount++;
		}

		IncrProcessed();
		USEMEM(node, tup->t_len);
		TRACEMEM(createrun);
		if (lasttuple != NULL && tuplecmp(tup, lasttuple,
										  &PS(node)->treeContext))
		{
			if (t_free <= 0)
			{
				t_free = 1000;
				memtuples = repalloc(memtuples,
							  (t_last + t_free + 1) * sizeof(HeapTuple));
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
	dumptuples(node, desttapenum);
	ENDRUN(node, desttapenum);		/* delimit the end of the run */

	t_last++;
	/* put tuples for the next run into leftist tree */
	if (t_last >= 1)
	{
		int			t;

		PsortTupDesc = PS(node)->treeContext.tupDesc;
		PsortKeys = PS(node)->treeContext.scanKeys;
		PsortNkeys = PS(node)->treeContext.nKeys;
		qsort(memtuples, t_last, sizeof(HeapTuple),
			  (int (*) (const void *, const void *)) _psort_cmp);
		for (t = t_last - 1; t >= 0; t--)
			puttuple(&PS(node)->Tuples, memtuples[t], 0, &PS(node)->treeContext);
	}

	pfree(memtuples);

	return !foundeor;
}

/*
 *		mergeruns		- merges all runs from input tapes
 *						  (polyphase merge Alg.D(D6)--Knuth, Vol.3, p271)
 *
 *		Returns:
 *				tape number of finished tape containing all tuples in order
 */
static int
mergeruns(Sort *node)
{
	struct tape *tp;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(PS(node)->using_tape_files);

	tp = PS(node)->Tape + PS(node)->TapeRange;
	merge(node, tp);
	while (--PS(node)->Level != 0)
	{
		/* rewind output tape to use as new input */
		LogicalTapeRewind(PS(node)->tapeset, tp->tp_tapenum, false);
		tp = tp->tp_prev;
		/* rewind new output tape and prepare it for write pass */
		LogicalTapeRewind(PS(node)->tapeset, tp->tp_tapenum, true);
		merge(node, tp);
	}
	/* freeze and rewind the final output tape */
	LogicalTapeFreeze(PS(node)->tapeset, tp->tp_tapenum);
	return tp->tp_tapenum;
}

/*
 *		merge			- handles a single merge of the tape
 *						  (polyphase merge Alg.D(D5)--Knuth, Vol.3, p271)
 */
static void
merge(Sort *node, struct tape * dest)
{
	HeapTuple	tup;
	struct tape *lasttp;		/* (TAPE[P]) */
	struct tape *tp;
	struct leftist *tuples;
	int			desttapenum;
	int			times;			/* runs left to merge */
	int			outdummy;		/* complete dummy runs */
	short		fromtape;
	unsigned int tuplen;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);
	Assert(PS(node)->using_tape_files);

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
	desttapenum = dest->tp_tapenum;
	while (times-- != 0)
	{							/* merge one run */
		tuples = NULL;
		if (PS(node)->TotalDummy == 0)
			for (tp = dest->tp_prev; tp != dest; tp = tp->tp_prev)
			{
				GETLEN(node, tuplen, tp->tp_tapenum);
				tup = ALLOCTUP(tuplen);
				USEMEM(node, tuplen);
				TRACEMEM(merge);
				GETTUP(node, tup, tuplen, tp->tp_tapenum);
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
					GETLEN(node, tuplen, tp->tp_tapenum);
					tup = ALLOCTUP(tuplen);
					USEMEM(node, tuplen);
					TRACEMEM(merge);
					GETTUP(node, tup, tuplen, tp->tp_tapenum);
					puttuple(&tuples, tup, tp - PS(node)->Tape,
							 &PS(node)->treeContext);
				}
			}
		}
		while (tuples != NULL)
		{
			/* possible optimization by using count in tuples */
			tup = gettuple(&tuples, &fromtape, &PS(node)->treeContext);
			PUTTUP(node, tup, desttapenum);
			FREEMEM(node, tup->t_len);
			FREE(tup);
			TRACEMEM(merge);
			if (TRYGETLEN(node, tuplen, PS(node)->Tape[fromtape].tp_tapenum))
			{
				tup = ALLOCTUP(tuplen);
				USEMEM(node, tuplen);
				TRACEMEM(merge);
				GETTUP(node, tup, tuplen, PS(node)->Tape[fromtape].tp_tapenum);
				puttuple(&tuples, tup, fromtape, &PS(node)->treeContext);
			}
		}
		ENDRUN(node, desttapenum);
	}
	PS(node)->TotalDummy += outdummy;
}

/*
 * dumptuples	- stores all the tuples remaining in tree to dest tape
 */
static void
dumptuples(Sort *node, int desttapenum)
{
	LeftistContext context = &PS(node)->treeContext;
	struct leftist **treep = &PS(node)->Tuples;
	struct leftist *tp;
	struct leftist *newp;
	HeapTuple	tup;

	Assert(PS(node)->using_tape_files);

	tp = *treep;
	while (tp != NULL)
	{
		tup = tp->lt_tuple;
		if (tp->lt_dist == 1)	/* lt_right == NULL */
			newp = tp->lt_left;
		else
			newp = lmerge(tp->lt_left, tp->lt_right, context);
		pfree(tp);
		PUTTUP(node, tup, desttapenum);
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
psort_grabtuple(Sort *node, bool *should_free)
{
	HeapTuple	tup;

	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	if (PS(node)->using_tape_files == true)
	{
		unsigned int tuplen;

		*should_free = true;
		if (ScanDirectionIsForward(node->plan.state->es_direction))
		{
			if (PS(node)->all_fetched)
				return NULL;
			if (TRYGETLEN(node, tuplen, PS(node)->psort_grab_tape))
			{
				tup = ALLOCTUP(tuplen);
				GETTUP(node, tup, tuplen, PS(node)->psort_grab_tape);
				return tup;
			}
			else
			{
				PS(node)->all_fetched = true;
				return NULL;
			}
		}
		/* Backward.
		 *
		 * if all tuples are fetched already then we return last tuple,
		 * else - tuple before last returned.
		 */
		if (PS(node)->all_fetched)
		{
			/*
			 * Assume seek position is pointing just past the zero tuplen
			 * at the end of file; back up and fetch last tuple's ending
			 * length word.  If seek fails we must have a completely empty
			 * file.
			 */
			if (! LogicalTapeBackspace(PS(node)->tapeset,
									   PS(node)->psort_grab_tape,
									   2 * sizeof(tlendummy)))
				return NULL;
			GETLEN(node, tuplen, PS(node)->psort_grab_tape);
			PS(node)->all_fetched = false;
		}
		else
		{
			/*
			 * Back up and fetch prev tuple's ending length word.
			 * If seek fails, assume we are at start of file.
			 */
			if (! LogicalTapeBackspace(PS(node)->tapeset,
									   PS(node)->psort_grab_tape,
									   sizeof(tlendummy)))
				return NULL;
			GETLEN(node, tuplen, PS(node)->psort_grab_tape);
			/*
			 * Back up to get ending length word of tuple before it.
			 */
			if (! LogicalTapeBackspace(PS(node)->tapeset,
									   PS(node)->psort_grab_tape,
									   tuplen + 2*sizeof(tlendummy)))
			{
				/* If fail, presumably the prev tuple is the first in the file.
				 * Back up so that it becomes next to read in forward direction
				 * (not obviously right, but that is what in-memory case does)
				 */
				if (! LogicalTapeBackspace(PS(node)->tapeset,
										   PS(node)->psort_grab_tape,
										   tuplen + sizeof(tlendummy)))
					elog(ERROR, "psort_grabtuple: too big last tuple len in backward scan");
				return NULL;
			}
			GETLEN(node, tuplen, PS(node)->psort_grab_tape);
		}

		/*
		 * Now we have the length of the prior tuple, back up and read it.
		 * Note: GETTUP expects we are positioned after the initial length
		 * word of the tuple, so back up to that point.
		 */
		if (! LogicalTapeBackspace(PS(node)->tapeset,
								   PS(node)->psort_grab_tape,
								   tuplen))
			elog(ERROR, "psort_grabtuple: too big tuple len in backward scan");
		tup = ALLOCTUP(tuplen);
		GETTUP(node, tup, tuplen, PS(node)->psort_grab_tape);
		return tup;
	}
	else
	{
		*should_free = false;
		if (ScanDirectionIsForward(node->plan.state->es_direction))
		{
			if (PS(node)->psort_current < PS(node)->tupcount)
				return PS(node)->memtuples[PS(node)->psort_current++];
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
			PS(node)->psort_current--;	/* last returned tuple */
			if (PS(node)->psort_current <= 0)
				return NULL;
		}
		return PS(node)->memtuples[PS(node)->psort_current - 1];
	}
}

/*
 *		psort_markpos	- saves current position in the merged sort file
 *
 * XXX I suspect these need to save & restore the all_fetched flag as well!
 */
void
psort_markpos(Sort *node)
{
	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	if (PS(node)->using_tape_files == true)
		LogicalTapeTell(PS(node)->tapeset,
						PS(node)->psort_grab_tape,
						& PS(node)->psort_saved,
						& PS(node)->psort_saved_offset);
	else
		PS(node)->psort_saved = PS(node)->psort_current;
}

/*
 *		psort_restorepos- restores current position in merged sort file to
 *						  last saved position
 */
void
psort_restorepos(Sort *node)
{
	Assert(node != (Sort *) NULL);
	Assert(PS(node) != (Psortstate *) NULL);

	if (PS(node)->using_tape_files == true)
	{
		if (! LogicalTapeSeek(PS(node)->tapeset,
							  PS(node)->psort_grab_tape,
							  PS(node)->psort_saved,
							  PS(node)->psort_saved_offset))
			elog(ERROR, "psort_restorepos failed");
	}
	else
		PS(node)->psort_current = PS(node)->psort_saved;
}

/*
 * psort_end
 *
 *	Release resources and clean up.
 */
void
psort_end(Sort *node)
{
	/* node->cleaned is probably redundant? */
	if (!node->cleaned && PS(node) != (Psortstate *) NULL)
	{
		if (PS(node)->tapeset)
			LogicalTapeSetClose(PS(node)->tapeset);
		if (PS(node)->memtuples)
			pfree(PS(node)->memtuples);

		/* XXX what about freeing leftist tree and tuples in memory? */

		NDirectFileRead += (int) ceil((double) PS(node)->BytesRead / BLCKSZ);
		NDirectFileWrite += (int) ceil((double) PS(node)->BytesWritten / BLCKSZ);

		pfree((void *) node->psortstate);
		node->psortstate = NULL;
		node->cleaned = TRUE;
	}
}

void
psort_rescan(Sort *node)
{

	/*
	 * If subnode is to be rescanned then free our previous results
	 */
	if (((Plan *) node)->lefttree->chgParam != NULL)
	{
		psort_end(node);
		node->cleaned = false;	/* huh? */
	}
	else if (PS(node) != (Psortstate *) NULL)
	{
		PS(node)->all_fetched = false;
		PS(node)->psort_current = 0;
		PS(node)->psort_saved = 0L;
		PS(node)->psort_saved_offset = 0;
		if (PS(node)->using_tape_files == true)
			LogicalTapeRewind(PS(node)->tapeset,
							  PS(node)->psort_grab_tape,
							  false);
	}

}

static int
_psort_cmp(HeapTuple *ltup, HeapTuple *rtup)
{
	Datum		lattr,
				rattr;
	int			nkey;
	int			result = 0;
	bool		isnull1,
				isnull2;

	for (nkey = 0; nkey < PsortNkeys && !result; nkey++)
	{
		lattr = heap_getattr(*ltup,
							 PsortKeys[nkey].sk_attno,
							 PsortTupDesc,
							 &isnull1);
		rattr = heap_getattr(*rtup,
							 PsortKeys[nkey].sk_attno,
							 PsortTupDesc,
							 &isnull2);
		if (isnull1)
		{
			if (!isnull2)
				result = 1;
		}
		else if (isnull2)
			result = -1;

		else if (PsortKeys[nkey].sk_flags & SK_COMMUTE)
		{
			if (!(result = -(long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (rattr, lattr)))
				result = (long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (lattr, rattr);
		}
		else if (!(result = -(long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (lattr, rattr)))
			result = (long) (*fmgr_faddr(&PsortKeys[nkey].sk_func)) (rattr, lattr);
	}
	return result;
}
