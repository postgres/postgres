/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of either heap tuples or index tuples
 * (and could fairly easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().  Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * See Knuth, volume 3, for more than you want to know about the external
 * sorting algorithm.  We divide the input into sorted runs using replacement
 * selection, in the form of a priority tree implemented as a heap
 * (essentially his Algorithm 5.2.3H), then merge the runs using polyphase
 * merge, Knuth's Algorithm 5.4.2D.  The logical "tapes" used by Algorithm D
 * are implemented by logtape.c, which avoids space wastage by recycling
 * disk space as soon as each block is read from its "tape".
 *
 * We do not form the initial runs using Knuth's recommended replacement
 * selection method (Algorithm 5.4.1R), because it uses a fixed number of
 * records in memory at all times.  Since we are dealing with tuples that
 * may vary considerably in size, we want to be able to vary the number of
 * records kept in memory to ensure full utilization of the allowed sort
 * memory space.  This is easily done by keeping a variable-size heap in
 * which the records of the current run are stored, plus a variable-size
 * unsorted array holding records that must go into the next run.
 *
 * The (approximate) amount of memory allowed for any one sort operation
 * is given in kilobytes by the external variable SortMem.  Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded SortMem.  If we reach the end of the input without
 * exceeding SortMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * SortMem, we construct a heap using Algorithm H and begin to emit tuples
 * into sorted runs in temporary tapes, emitting just enough tuples at each
 * step to get back within the SortMem limit.  New tuples are added to the
 * heap if they can go into the current run, else they are temporarily added
 * to the unsorted array.  Whenever the heap empties, we construct a new heap
 * from the current contents of the unsorted array, and begin a new run with a
 * new output tape (selected per Algorithm D).  After the end of the input
 * is reached, we dump out remaining tuples in memory into a final run
 * (or two), then merge the runs using Algorithm D.
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.  When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
 * to one run per logical tape.  The final merge is then performed
 * on-the-fly as the caller repeatedly calls tuplesort_gettuple; this
 * saves one cycle of writing all the data out to disk and reading it in.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/tuplesort.c,v 1.1 1999/10/17 22:15:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "miscadmin.h"
#include "utils/logtape.h"
#include "utils/tuplesort.h"

/*
 * Possible states of a Tuplesort object.  These denote the states that
 * persist between calls of Tuplesort routines.
 */
typedef enum
{
	TSS_INITIAL,		/* Loading tuples; still within memory limit */
	TSS_BUILDRUNS,		/* Loading tuples; writing to tape */
	TSS_SORTEDINMEM,	/* Sort completed entirely in memory */
	TSS_SORTEDONTAPE,	/* Sort completed, final run is on tape */
	TSS_FINALMERGE		/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * We use a seven-tape polyphase merge, which is the "sweet spot" on the
 * tapes-to-passes curve according to Knuth's figure 70 (section 5.4.2).
 */
#define MAXTAPES		7				/* Knuth's T */
#define TAPERANGE		(MAXTAPES-1)	/* Knuth's P */

/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate
{
	TupSortStatus status;		/* enumerated value as shown above */
	bool		randomAccess;	/* did caller request random access? */
	long		availMem;		/* remaining memory available, in bytes */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

	/*
	 * These function pointers decouple the routines that must know what kind
	 * of tuple we are sorting from the routines that don't need to know it.
	 * They are set up by the tuplesort_begin_xxx routines.
	 *
	 * Function to compare two tuples; result is per qsort() convention,
	 * ie, <0, 0, >0 according as a<b, a=b, a>b.
	 */
	int (*comparetup) (Tuplesortstate *state, const void *a, const void *b);
	/*
	 * Function to copy a supplied input tuple into palloc'd space.
	 * (NB: we assume that a single pfree() is enough to release the tuple
	 * later, so the representation must be "flat" in one palloc chunk.)
	 * state->availMem must be decreased by the amount of space used.
	 */
	void * (*copytup) (Tuplesortstate *state, void *tup);
	/*
	 * Function to write a stored tuple onto tape.  The representation of
	 * the tuple on tape need not be the same as it is in memory; requirements
	 * on the tape representation are given below.  After writing the tuple,
	 * pfree() it, and increase state->availMem by the amount of memory space
	 * thereby released.
	 */
	void (*writetup) (Tuplesortstate *state, int tapenum, void *tup);
	/*
	 * Function to read a stored tuple from tape back into memory.
	 * 'len' is the already-read length of the stored tuple.  Create and
	 * return a palloc'd copy, and decrease state->availMem by the amount
	 * of memory space consumed.
	 */
	void * (*readtup) (Tuplesortstate *state, int tapenum, unsigned int len);

	/*
	 * This array holds "unsorted" tuples during the input phases.
	 * If we are able to complete the sort in memory, it holds the
	 * final sorted result as well.
	 */
	void	  **memtuples;		/* array of pointers to palloc'd tuples */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */

	/*
	 * This array holds the partially-sorted "heap" of tuples that will go
	 * out in the current run during BUILDRUNS state.  While completing
	 * the sort, we use it to merge runs of tuples from input tapes.
	 * It is never allocated unless we need to use tapes.
	 */
	void	  **heaptuples;		/* array of pointers to palloc'd tuples */
	int			heaptupcount;	/* number of tuples currently present */
	int			heaptupsize;	/* allocated length of heaptuples array */
	/*
	 * While merging, this array holds the actual number of the input tape
	 * that each tuple in heaptuples[] came from.
	 */
	int		   *heapsrctapes;

	/*
	 * Variables for Algorithm D.  Note that destTape is a "logical" tape
	 * number, ie, an index into the tp_xxx[] arrays.  Be careful to keep
	 * "logical" and "actual" tape numbers straight!
	 */
	int			Level;			/* Knuth's l */
	int			destTape;		/* current output tape (Knuth's j, less 1) */
	int			tp_fib[MAXTAPES]; /* Target Fibonacci run counts (A[]) */
	int			tp_runs[MAXTAPES];	/* # of real runs on each tape */
	int			tp_dummy[MAXTAPES];	/* # of dummy runs for each tape (D[]) */
	int			tp_tapenum[MAXTAPES]; /* Actual tape numbers (TAPE[]) */

	bool		multipleRuns;	/* T if we have created more than 1 run */

	/*
	 * These variables are used after completion of sorting to keep track
	 * of the next tuple to return.  (In the tape case, the tape's current
	 * read position is also critical state.)
	 */
	int			result_tape;	/* actual tape number of finished output */
	int			current;		/* array index (only used if SORTEDINMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	long		markpos_block;	/* tape block# (only used if SORTEDONTAPE) */
	int			markpos_offset;	/* saved "current", or offset in tape block */
	bool		markpos_eof;	/* saved "eof_reached" */

	/*
	 * These variables are specific to the HeapTuple case; they are set
	 * by tuplesort_begin_heap and used only by the HeapTuple routines.
	 */
	TupleDesc	tupDesc;
	int			nKeys;
	ScanKey		scanKeys;

	/*
	 * These variables are specific to the IndexTuple case; they are set
	 * by tuplesort_begin_index and used only by the IndexTuple routines.
	 */
	Relation	indexRel;
	bool		enforceUnique;	/* complain if we find duplicate tuples */
};

#define COMPARETUP(state,a,b)	((*(state)->comparetup) (state, a, b))
#define COPYTUP(state,tup)	((*(state)->copytup) (state, tup))
#define WRITETUP(state,tape,tup)	((*(state)->writetup) (state, tape, tup))
#define READTUP(state,tape,len)	((*(state)->readtup) (state, tape, len))
#define LACKMEM(state)		((state)->availMem < 0)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))

/*--------------------
 *
 * NOTES about on-tape representation of tuples:
 *
 * We require the first "unsigned int" of a stored tuple to be the total size
 * on-tape of the tuple, including itself (so it is never zero; an all-zero
 * unsigned int is used to delimit runs).  The remainder of the stored tuple
 * may or may not match the in-memory representation of the tuple ---
 * any conversion needed is the job of the writetup and readtup routines.
 *
 * If state->randomAccess is true, then the stored representation of the
 * tuple must be followed by another "unsigned int" that is a copy of the
 * length --- so the total tape space used is actually sizeof(unsigned int)
 * more than the stored length value.  This allows read-backwards.  When
 * randomAccess is not true, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplesortstate record, if needed.  They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should elog() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space requested for tuples against the SortMem limit.
 * Fixed-size space (primarily the LogicalTapeSet I/O buffers) is not
 * counted, nor do we count the variable-size memtuples and heaptuples
 * arrays.  (Even though those could grow pretty large, they should be
 * small compared to the tuples proper, so this is not unreasonable.)
 *
 * The major deficiency in this approach is that it ignores palloc overhead.
 * The memory space actually allocated for a palloc chunk is always more
 * than the request size, and could be considerably more (as much as 2X
 * larger, in the current aset.c implementation).  So the space used could
 * be considerably more than SortMem says.
 *
 * One way to fix this is to add a memory management function that, given
 * a pointer to a palloc'd chunk, returns the actual space consumed by the
 * chunk.  This would be very easy in the current aset.c module, but I'm
 * hesitant to do it because it might be unpleasant to support in future
 * implementations of memory management.  (For example, a direct
 * implementation of palloc as malloc could not support such a function
 * portably.)
 *
 * A cruder answer is just to apply a fudge factor, say by initializing
 * availMem to only three-quarters of what SortMem indicates.  This is
 * probably the right answer if anyone complains that SortMem is not being
 * obeyed very faithfully.
 *
 *--------------------
 */

static Tuplesortstate *tuplesort_begin_common(bool randomAccess);
static void inittapes(Tuplesortstate *state);
static void selectnewtape(Tuplesortstate *state);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static void beginrun(Tuplesortstate *state);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void tuplesort_heap_insert(Tuplesortstate *state, void *tuple,
								  int tapenum);
static void tuplesort_heap_siftup(Tuplesortstate *state);
static unsigned int getlen(Tuplesortstate *state, int tapenum, bool eofOK);
static void markrunend(Tuplesortstate *state, int tapenum);
static int qsort_comparetup(const void *a, const void *b);
static int comparetup_heap(Tuplesortstate *state,
						   const void *a, const void *b);
static void *copytup_heap(Tuplesortstate *state, void *tup);
static void writetup_heap(Tuplesortstate *state, int tapenum, void *tup);
static void *readtup_heap(Tuplesortstate *state, int tapenum,
						  unsigned int len);
static int comparetup_index(Tuplesortstate *state,
							const void *a, const void *b);
static void *copytup_index(Tuplesortstate *state, void *tup);
static void writetup_index(Tuplesortstate *state, int tapenum, void *tup);
static void *readtup_index(Tuplesortstate *state, int tapenum,
						   unsigned int len);

/*
 * Since qsort(3) will not pass any context info to qsort_comparetup(),
 * we have to use this ugly static variable.  It is set to point to the
 * active Tuplesortstate object just before calling qsort.  It should
 * not be used directly by anything except qsort_comparetup().
 */
static Tuplesortstate *qsort_tuplesortstate;


/*
 *		tuplesort_begin_xxx
 *
 * Initialize for a tuple sort operation.
 *
 * After calling tuplesort_begin, the caller should call tuplesort_puttuple
 * zero or more times, then call tuplesort_performsort when all the tuples
 * have been supplied.  After performsort, retrieve the tuples in sorted
 * order by calling tuplesort_gettuple until it returns NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * Call tuplesort_end to terminate the operation and release memory/disk space.
 */

static Tuplesortstate *
tuplesort_begin_common(bool randomAccess)
{
	Tuplesortstate *state;

	state = (Tuplesortstate *) palloc(sizeof(Tuplesortstate));

	MemSet((char *) state, 0, sizeof(Tuplesortstate));

	state->status = TSS_INITIAL;
	state->randomAccess = randomAccess;
	state->availMem = SortMem * 1024L;
	state->tapeset = NULL;

	state->memtupcount = 0;
	state->memtupsize = 1024;	/* initial guess */
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	state->heaptuples = NULL;	/* until and unless needed */
	state->heaptupcount = 0;
	state->heaptupsize = 0;
	state->heapsrctapes = NULL;

	/* Algorithm D variables will be initialized by inittapes, if needed */

	state->result_tape = -1;	/* flag that result tape has not been formed */

	return state;
}

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, ScanKey keys,
					 bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(randomAccess);

	AssertArg(nkeys >= 1);
	AssertArg(keys[0].sk_attno != 0);
	AssertArg(keys[0].sk_procedure != 0);

	state->comparetup = comparetup_heap;
	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	state->tupDesc = tupDesc;
	state->nKeys = nkeys;
	state->scanKeys = keys;

	return state;
}

Tuplesortstate *
tuplesort_begin_index(Relation indexRel,
					  bool enforceUnique,
					  bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(randomAccess);

	state->comparetup = comparetup_index;
	state->copytup = copytup_index;
	state->writetup = writetup_index;
	state->readtup = readtup_index;

	state->indexRel = indexRel;
	state->enforceUnique = enforceUnique;

	return state;
}

/*
 * tuplesort_end
 *
 *	Release resources and clean up.
 */
void
tuplesort_end(Tuplesortstate *state)
{
	int		i;

	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);
	if (state->memtuples)
	{
		for (i = 0; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	if (state->heaptuples)
	{
		for (i = 0; i < state->heaptupcount; i++)
			pfree(state->heaptuples[i]);
		pfree(state->heaptuples);
	}
	if (state->heapsrctapes)
		pfree(state->heapsrctapes);
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 */
void
tuplesort_puttuple(Tuplesortstate *state, void *tuple)
{
	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 */
	tuple = COPYTUP(state, tuple);

	switch (state->status)
	{
		case TSS_INITIAL:
			/*
			 * Save the copied tuple into the unsorted array.
			 */
			if (state->memtupcount >= state->memtupsize)
			{
				/* Grow the unsorted array as needed. */
				state->memtupsize *= 2;
				state->memtuples = (void **)
					repalloc(state->memtuples,
							 state->memtupsize * sizeof(void *));
			}
			state->memtuples[state->memtupcount++] = tuple;
			/*
			 * Done if we still fit in available memory.
			 */
			if (! LACKMEM(state))
				return;
			/*
			 * Nope; time to switch to tape-based operation.
			 */
			inittapes(state);
			beginrun(state);
			/*
			 * Dump tuples until we are back under the limit.
			 */
			dumptuples(state, false);
			break;
		case TSS_BUILDRUNS:
			/*
			 * Insert the copied tuple into the heap if it can go into the
			 * current run; otherwise add it to the unsorted array, whence
			 * it will go into the next run.
			 *
			 * The tuple can go into the current run if it is >= the first
			 * not-yet-output tuple.  (Actually, it could go into the current
			 * run if it is >= the most recently output tuple ... but that
			 * would require keeping around the tuple we last output, and
			 * it's simplest to let writetup free the tuple when written.)
			 *
			 * Note there will always be at least one tuple in the heap
			 * at this point; see dumptuples.
			 */
			Assert(state->heaptupcount > 0);
			if (COMPARETUP(state, tuple, state->heaptuples[0]) >= 0)
			{
				tuplesort_heap_insert(state, tuple, 0);
			}
			else
			{
				if (state->memtupcount >= state->memtupsize)
				{
					/* Grow the unsorted array as needed. */
					state->memtupsize *= 2;
					state->memtuples = (void **)
						repalloc(state->memtuples,
								 state->memtupsize * sizeof(void *));
				}
				state->memtuples[state->memtupcount++] = tuple;
			}
			/*
			 * If we are over the memory limit, dump tuples till we're under.
			 */
			dumptuples(state, false);
			break;
		default:
			elog(ERROR, "tuplesort_puttuple: invalid state");
			break;
	}
}

/*
 * All tuples have been provided; finish the sort.
 */
void
tuplesort_performsort(Tuplesortstate *state)
{
	switch (state->status)
	{
		case TSS_INITIAL:
			/*
			 * We were able to accumulate all the tuples within the
			 * allowed amount of memory.  Just qsort 'em and we're done.
			 */
			if (state->memtupcount > 1)
			{
				qsort_tuplesortstate = state;
				qsort((void *) state->memtuples, state->memtupcount,
					  sizeof(void *), qsort_comparetup);
			}
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			state->status = TSS_SORTEDINMEM;
			break;
		case TSS_BUILDRUNS:
			/*
			 * Finish tape-based sort.  First, flush all tuples remaining
			 * in memory out to tape; then merge until we have a single
			 * remaining run (or, if !randomAccess, one run per tape).
			 * Note that mergeruns sets the correct status.
			 */
			dumptuples(state, true);
			mergeruns(state);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "tuplesort_performsort: invalid state");
			break;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.  If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
void *
tuplesort_gettuple(Tuplesortstate *state, bool forward,
				   bool *should_free)
{
	unsigned int	tuplen;
	void		   *tup;

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			Assert(forward || state->randomAccess);
			*should_free = false;
			if (forward)
			{
				if (state->current < state->memtupcount)
					return state->memtuples[state->current++];
				state->eof_reached = true;
				return NULL;
			}
			else
			{
				if (state->current <= 0)
					return NULL;
				/*
				 * if all tuples are fetched already then we return last tuple,
				 * else - tuple before last returned.
				 */
				if (state->eof_reached)
					state->eof_reached = false;
				else
				{
					state->current--; /* last returned tuple */
					if (state->current <= 0)
						return NULL;
				}
				return state->memtuples[state->current - 1];
			}
			break;

		case TSS_SORTEDONTAPE:
			Assert(forward || state->randomAccess);
			*should_free = true;
			if (forward)
			{
				if (state->eof_reached)
					return NULL;
				if ((tuplen = getlen(state, state->result_tape, true)) != 0)
				{
					tup = READTUP(state, state->result_tape, tuplen);
					return tup;
				}
				else
				{
					state->eof_reached = true;
					return NULL;
				}
			}
			/* Backward.
			 *
			 * if all tuples are fetched already then we return last tuple,
			 * else - tuple before last returned.
			 */
			if (state->eof_reached)
			{
				/*
				 * Seek position is pointing just past the zero tuplen
				 * at the end of file; back up to fetch last tuple's ending
				 * length word.  If seek fails we must have a completely empty
				 * file.
				 */
				if (! LogicalTapeBackspace(state->tapeset,
										   state->result_tape,
										   2 * sizeof(unsigned int)))
					return NULL;
				state->eof_reached = false;
			}
			else
			{
				/*
				 * Back up and fetch previously-returned tuple's ending length
				 * word.  If seek fails, assume we are at start of file.
				 */
				if (! LogicalTapeBackspace(state->tapeset,
										   state->result_tape,
										   sizeof(unsigned int)))
					return NULL;
				tuplen = getlen(state, state->result_tape, false);
				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (! LogicalTapeBackspace(state->tapeset,
										   state->result_tape,
										   tuplen + 2 * sizeof(unsigned int)))
				{
					/* If that fails, presumably the prev tuple is the first
					 * in the file.  Back up so that it becomes next to read
					 * in forward direction (not obviously right, but that is
					 * what in-memory case does).
					 */
					if (! LogicalTapeBackspace(state->tapeset,
											   state->result_tape,
											   tuplen + sizeof(unsigned int)))
						elog(ERROR, "tuplesort_gettuple: bogus tuple len in backward scan");
					return NULL;
				}
			}

			tuplen = getlen(state, state->result_tape, false);
			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			if (! LogicalTapeBackspace(state->tapeset,
									   state->result_tape,
									   tuplen))
				elog(ERROR, "tuplesort_gettuple: bogus tuple len in backward scan");
			tup = READTUP(state, state->result_tape, tuplen);
			return tup;

		case TSS_FINALMERGE:
			Assert(forward);
			*should_free = true;
			/*
			 * This code should match the inner loop of mergeonerun().
			 */
			if (state->heaptupcount > 0)
			{
				int		srcTape = state->heapsrctapes[0];

				tup = state->heaptuples[0];
				tuplesort_heap_siftup(state);
				if ((tuplen = getlen(state, srcTape, true)) != 0)
				{
					void   *newtup = READTUP(state, srcTape, tuplen);
					tuplesort_heap_insert(state, newtup, srcTape);
				}
				return tup;
			}
			return NULL;

		default:
			elog(ERROR, "tuplesort_gettuple: invalid state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we don't have room to sort in memory.
 */
static void
inittapes(Tuplesortstate *state)
{
	int			j;

	state->tapeset = LogicalTapeSetCreate(MAXTAPES);

	/*
	 * Initialize heaptuples array slightly larger than current memtuples
	 * usage; memtupcount is probably a good guess at how many tuples we
	 * will be able to have in the heap at once.
	 */
	state->heaptupcount = 0;
	state->heaptupsize = state->memtupcount + state->memtupcount / 4;
	state->heaptuples = (void **) palloc(state->heaptupsize * sizeof(void *));

	/*
	 * Initialize variables of Algorithm D (step D1).
	 */
	for (j = 0; j < MAXTAPES; j++)
	{
		state->tp_fib[j] = 1;
		state->tp_runs[j] = 0;
		state->tp_dummy[j] = 1;
		state->tp_tapenum[j] = j;
	}
	state->tp_fib[TAPERANGE] = 0;
	state->tp_dummy[TAPERANGE] = 0;

	state->Level = 1;
	state->destTape = 0;

	state->multipleRuns = false;

	state->status = TSS_BUILDRUNS;
}

/*
 * selectnewtape -- select new tape for new initial run.
 *
 * This is called after finishing a run when we know another run
 * must be started.  This implements steps D3, D4 of Algorithm D.
 */
static void
selectnewtape(Tuplesortstate *state)
{
	int		j;
	int		a;

	/* We now have at least two initial runs */
	state->multipleRuns = true;

	/* Step D3: advance j (destTape) */
	if (state->tp_dummy[state->destTape] < state->tp_dummy[state->destTape+1])
	{
		state->destTape++;
		return;
	}
	if (state->tp_dummy[state->destTape] != 0)
	{
		state->destTape = 0;
		return;
	}

	/* Step D4: increase level */
	state->Level++;
	a = state->tp_fib[0];
	for (j = 0; j < TAPERANGE; j++)
	{
		state->tp_dummy[j] = a + state->tp_fib[j+1] - state->tp_fib[j];
		state->tp_fib[j] = a + state->tp_fib[j+1];
	}
	state->destTape = 0;
}

/*
 * mergeruns -- merge all the completed initial runs.
 *
 * This implements steps D5, D6 of Algorithm D.  All input data has
 * already been written to initial runs on tape (see dumptuples).
 */
static void
mergeruns(Tuplesortstate *state)
{
	int		tapenum,
			svTape,
			svRuns,
			svDummy;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0 && state->heaptupcount == 0);
	/*
	 * If we produced only one initial run (quite likely if the total
	 * data volume is between 1X and 2X SortMem), we can just use that
	 * tape as the finished output, rather than doing a useless merge.
	 */
	if (! state->multipleRuns)
	{
		state->result_tape = state->tp_tapenum[state->destTape];
		/* must freeze and rewind the finished output tape */
		LogicalTapeFreeze(state->tapeset, state->result_tape);
		state->status = TSS_SORTEDONTAPE;
		return;
	}

	/* End of step D2: rewind all output tapes to prepare for merging */
	for (tapenum = 0; tapenum < TAPERANGE; tapenum++)
		LogicalTapeRewind(state->tapeset, tapenum, false);

	for (;;)
	{
		/* Step D5: merge runs onto tape[T] until tape[P] is empty */
		while (state->tp_runs[TAPERANGE-1] || state->tp_dummy[TAPERANGE-1])
		{
			bool	allDummy = true;
			bool	allOneRun = true;

			for (tapenum = 0; tapenum < TAPERANGE; tapenum++)
			{
				if (state->tp_dummy[tapenum] == 0)
					allDummy = false;
				if (state->tp_runs[tapenum] + state->tp_dummy[tapenum] != 1)
					allOneRun = false;
			}
			/*
			 * If we don't have to produce a materialized sorted tape,
			 * quit as soon as we're down to one real/dummy run per tape.
			 */
			if (! state->randomAccess && allOneRun)
			{
				Assert(! allDummy);
				/* Initialize for the final merge pass */
				beginmerge(state);
				state->status = TSS_FINALMERGE;
				return;
			}
			if (allDummy)
			{	
				state->tp_dummy[TAPERANGE]++;
				for (tapenum = 0; tapenum < TAPERANGE; tapenum++)
					state->tp_dummy[tapenum]--;
			}
			else
			{
				mergeonerun(state);
			}
		}
		/* Step D6: decrease level */
		if (--state->Level == 0)
			break;
		/* rewind output tape T to use as new input */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[TAPERANGE],
						  false);
		/* rewind used-up input tape P, and prepare it for write pass */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[TAPERANGE-1],
						  true);
		state->tp_runs[TAPERANGE-1] = 0;
		/* reassign tape units per step D6; note we no longer care about A[] */
		svTape = state->tp_tapenum[TAPERANGE];
		svDummy = state->tp_dummy[TAPERANGE];
		svRuns = state->tp_runs[TAPERANGE];
		for (tapenum = TAPERANGE; tapenum > 0; tapenum--)
		{
			state->tp_tapenum[tapenum] = state->tp_tapenum[tapenum-1];
			state->tp_dummy[tapenum] = state->tp_dummy[tapenum-1];
			state->tp_runs[tapenum] = state->tp_runs[tapenum-1];
		}
		state->tp_tapenum[0] = svTape;
		state->tp_dummy[0] = svDummy;
		state->tp_runs[0] = svRuns;
	}
	/*
	 * Done.  Knuth says that the result is on TAPE[1], but since we exited
	 * the loop without performing the last iteration of step D6, we have not
	 * rearranged the tape unit assignment, and therefore the result is on
	 * TAPE[T].  We need to do it this way so that we can freeze the final
	 * output tape while rewinding it.  The last iteration of step D6 would
	 * be a waste of cycles anyway...
	 */
	state->result_tape = state->tp_tapenum[TAPERANGE];
	LogicalTapeFreeze(state->tapeset, state->result_tape);
	state->status = TSS_SORTEDONTAPE;
}

/*
 * Merge one run from each input tape, except ones with dummy runs.
 *
 * This is the inner loop of Algorithm D step D5.  We know that the
 * output tape is TAPE[T].
 */
static void
mergeonerun(Tuplesortstate *state)
{
	int				destTape = state->tp_tapenum[TAPERANGE];
	int				srcTape;
	unsigned int	tuplen;
	void		   *tup;

	/*
	 * Start the merge by loading one tuple from each active source tape
	 * into the heap.  We can also decrease the input run/dummy run counts.
	 */
	beginmerge(state);

	/*
	 * Execute merge by repeatedly extracting lowest tuple in heap,
	 * writing it out, and replacing it with next tuple from same tape
	 * (if there is another one).
	 */
	while (state->heaptupcount > 0)
	{
		WRITETUP(state, destTape, state->heaptuples[0]);
		srcTape = state->heapsrctapes[0];
		tuplesort_heap_siftup(state);
		if ((tuplen = getlen(state, srcTape, true)) != 0)
		{
			tup = READTUP(state, srcTape, tuplen);
			tuplesort_heap_insert(state, tup, srcTape);
		}
	}

	/*
	 * When the heap empties, we're done.  Write an end-of-run marker
	 * on the output tape, and increment its count of real runs.
	 */
	markrunend(state, destTape);
	state->tp_runs[TAPERANGE]++;
}

/*
 * beginmerge - initialize for a merge pass
 *
 * We load the first tuple from each nondummy input run into the heap.
 * We also decrease the counts of real and dummy runs for each tape.
 */
static void
beginmerge(Tuplesortstate *state)
{
	int				tapenum;
	int				srcTape;
	unsigned int	tuplen;
	void		   *tup;

	Assert(state->heaptuples != NULL && state->heaptupcount == 0);
	if (state->heapsrctapes == NULL)
		state->heapsrctapes = (int *) palloc(MAXTAPES * sizeof(int));

	for (tapenum = 0; tapenum < TAPERANGE; tapenum++)
	{
		if (state->tp_dummy[tapenum] > 0)
		{
			state->tp_dummy[tapenum]--;
		}
		else
		{
			Assert(state->tp_runs[tapenum] > 0);
			state->tp_runs[tapenum]--;
			srcTape = state->tp_tapenum[tapenum];
			tuplen = getlen(state, srcTape, false);
			tup = READTUP(state, srcTape, tuplen);
			tuplesort_heap_insert(state, tup, srcTape);
		}
	}

}

/*
 * beginrun - start a new initial run
 *
 * The tuples presently in the unsorted memory array are moved into
 * the heap.
 */
static void
beginrun(Tuplesortstate *state)
{
	int		i;

	Assert(state->heaptupcount == 0 && state->memtupcount > 0);
	for (i = 0; i < state->memtupcount; i++)
		tuplesort_heap_insert(state, state->memtuples[i], 0);
	state->memtupcount = 0;
}

/*
 * dumptuples - remove tuples from heap and write to tape
 *
 * When alltuples = false, dump only enough tuples to get under the
 * availMem limit (and leave at least one tuple in the heap in any case,
 * since puttuple assumes it always has a tuple to compare to).
 *
 * When alltuples = true, dump everything currently in memory.
 * (This case is only used at end of input data.)
 *
 * If we empty the heap, then start a new run using the tuples that
 * have accumulated in memtuples[] (if any).
 */
static void
dumptuples(Tuplesortstate *state, bool alltuples)
{
	while (alltuples ||
		   (LACKMEM(state) &&
			(state->heaptupcount > 0 || state->memtupcount > 0)))
	{
		/*
		 * Dump the heap's frontmost entry, and sift up to remove it
		 * from the heap.
		 */
		Assert(state->heaptupcount > 0);
		WRITETUP(state, state->tp_tapenum[state->destTape],
				 state->heaptuples[0]);
		tuplesort_heap_siftup(state);
		/*
		 * If the heap is now empty, we've finished a run.
		 */
		if (state->heaptupcount == 0)
		{
			markrunend(state, state->tp_tapenum[state->destTape]);
			state->tp_runs[state->destTape]++;
			state->tp_dummy[state->destTape]--;	/* per Alg D step D2 */
			if (state->memtupcount == 0)
				break;			/* all input data has been written to tape */
			/* Select new output tape and start a new run */
			selectnewtape(state);
			beginrun(state);
		}
	}
}

/*
 * tuplesort_rescan		- rewind and replay the scan
 */
void
tuplesort_rescan(Tuplesortstate *state)
{
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeRewind(state->tapeset,
							  state->result_tape,
							  false);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "tuplesort_rescan: invalid state");
			break;
	}
}

/*
 * tuplesort_markpos	- saves current position in the merged sort file
 */
void
tuplesort_markpos(Tuplesortstate *state)
{
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->markpos_offset = state->current;
			state->markpos_eof = state->eof_reached;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeTell(state->tapeset,
							state->result_tape,
							& state->markpos_block,
							& state->markpos_offset);
			state->markpos_eof = state->eof_reached;
			break;
		default:
			elog(ERROR, "tuplesort_markpos: invalid state");
			break;
	}
}

/*
 * tuplesort_restorepos	- restores current position in merged sort file to
 *						  last saved position
 */
void
tuplesort_restorepos(Tuplesortstate *state)
{
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_SORTEDONTAPE:
			if (! LogicalTapeSeek(state->tapeset,
								  state->result_tape,
								  state->markpos_block,
								  state->markpos_offset))
				elog(ERROR, "tuplesort_restorepos failed");
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "tuplesort_restorepos: invalid state");
			break;
	}
}


/*
 * Heap manipulation routines, per Knuth's Algorithm 5.2.3H.
 */

/*
 * Insert a new tuple into an empty or existing heap, maintaining the
 * heap invariant.  The heap lives in state->heaptuples[].  Also, if
 * state->heapsrctapes is not NULL, we store each tuple's source tapenum
 * in the corresponding element of state->heapsrctapes[].
 */
static void
tuplesort_heap_insert(Tuplesortstate *state, void *tuple,
					  int tapenum)
{
	int		j;

	/*
	 * Make sure heaptuples[] can handle another entry.
	 * NOTE: we do not enlarge heapsrctapes[]; it's supposed
	 * to be big enough when created.
	 */
	if (state->heaptupcount >= state->heaptupsize)
	{
		/* Grow the unsorted array as needed. */
		state->heaptupsize *= 2;
		state->heaptuples = (void **)
			repalloc(state->heaptuples,
					 state->heaptupsize * sizeof(void *));
	}
	/*
	 * Sift-up the new entry, per Knuth 5.2.3 exercise 16.
	 * Note that Knuth is using 1-based array indexes, not 0-based.
	 */
	j = state->heaptupcount++;
	while (j > 0) {
		int		i = (j-1) >> 1;

		if (COMPARETUP(state, tuple, state->heaptuples[i]) >= 0)
			break;
		state->heaptuples[j] = state->heaptuples[i];
		if (state->heapsrctapes)
			state->heapsrctapes[j] = state->heapsrctapes[i];
		j = i;
	}
	state->heaptuples[j] = tuple;
	if (state->heapsrctapes)
		state->heapsrctapes[j] = tapenum;
}

/*
 * The tuple at state->heaptuples[0] has been removed from the heap.
 * Decrement heaptupcount, and sift up to maintain the heap invariant.
 */
static void
tuplesort_heap_siftup(Tuplesortstate *state)
{
	void  **heaptuples = state->heaptuples;
	void   *tuple;
	int		i,
			n;

	if (--state->heaptupcount <= 0)
		return;
	n = state->heaptupcount;
	tuple = heaptuples[n];		/* tuple that must be reinserted */
	i = 0;						/* i is where the "hole" is */
    for (;;) {
		int		j = 2*i + 1;

		if (j >= n)
			break;
		if (j+1 < n &&
			COMPARETUP(state, heaptuples[j], heaptuples[j+1]) > 0)
			j++;
		if (COMPARETUP(state, tuple, heaptuples[j]) <= 0)
			break;
		heaptuples[i] = heaptuples[j];
		if (state->heapsrctapes)
			state->heapsrctapes[i] = state->heapsrctapes[j];
		i = j;
    }
    heaptuples[i] = tuple;
	if (state->heapsrctapes)
		state->heapsrctapes[i] = state->heapsrctapes[n];
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplesortstate *state, int tapenum, bool eofOK)
{
	unsigned int	len;

	if (LogicalTapeRead(state->tapeset, tapenum, (void *) &len,
						sizeof(len)) != sizeof(len))
		elog(ERROR, "tuplesort: unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "tuplesort: unexpected end of data");
	return len;
}

static void
markrunend(Tuplesortstate *state, int tapenum)
{
	unsigned int	len = 0;

	LogicalTapeWrite(state->tapeset, tapenum, (void *) &len, sizeof(len));
}


/*
 * qsort interface
 */

static int
qsort_comparetup(const void *a, const void *b)
{
	/* The passed pointers are pointers to void * ... */

	return COMPARETUP(qsort_tuplesortstate, * (void **) a, * (void **) b);
}


/*
 * Routines specialized for HeapTuple case
 */

static int
comparetup_heap(Tuplesortstate *state, const void *a, const void *b)
{
	HeapTuple	ltup = (HeapTuple) a;
	HeapTuple	rtup = (HeapTuple) b;
	int			nkey;

	for (nkey = 0; nkey < state->nKeys; nkey++)
	{
		ScanKey		scanKey = state->scanKeys + nkey;
		Datum		lattr,
					rattr;
		bool		isnull1,
					isnull2;
		int			result;

		lattr = heap_getattr(ltup,
							 scanKey->sk_attno,
							 state->tupDesc,
							 &isnull1);
		rattr = heap_getattr(rtup,
							 scanKey->sk_attno,
							 state->tupDesc,
							 &isnull2);
		if (isnull1)
		{
			if (!isnull2)
				return 1;		/* NULL sorts after non-NULL */
		}
		else if (isnull2)
			return -1;
		else if (scanKey->sk_flags & SK_COMMUTE)
		{
			if (!(result = - (int) (*fmgr_faddr(&scanKey->sk_func)) (rattr, lattr)))
				result = (int) (*fmgr_faddr(&scanKey->sk_func)) (lattr, rattr);
			if (result)
				return result;
		}
		else
		{
			if (!(result = - (int) (*fmgr_faddr(&scanKey->sk_func)) (lattr, rattr)))
				result = (int) (*fmgr_faddr(&scanKey->sk_func)) (rattr, lattr);
			if (result)
				return result;
		}
	}

	return 0;
}

static void *
copytup_heap(Tuplesortstate *state, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;

	USEMEM(state, HEAPTUPLESIZE + tuple->t_len);
	return (void *) heap_copytuple(tuple);
}

/*
 * We don't bother to write the HeapTupleData part of the tuple.
 */

static void
writetup_heap(Tuplesortstate *state, int tapenum, void *tup)
{
	HeapTuple		tuple = (HeapTuple) tup;
	unsigned int	tuplen;

	tuplen = tuple->t_len + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void*) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void*) tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void*) &tuplen, sizeof(tuplen));

	FREEMEM(state, HEAPTUPLESIZE + tuple->t_len);
	pfree(tuple);
}

static void *
readtup_heap(Tuplesortstate *state, int tapenum, unsigned int len)
{
	unsigned int	tuplen = len - sizeof(unsigned int) + HEAPTUPLESIZE;
	HeapTuple		tuple = (HeapTuple) palloc(tuplen);

	USEMEM(state, tuplen);
	/* reconstruct the HeapTupleData portion */
	tuple->t_len = len - sizeof(unsigned int);
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_data = (HeapTupleHeader) (((char *) tuple) + HEAPTUPLESIZE);
	/* read in the tuple proper */
	if (LogicalTapeRead(state->tapeset, tapenum, (void *) tuple->t_data,
						tuple->t_len) != tuple->t_len)
		elog(ERROR, "tuplesort: unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (LogicalTapeRead(state->tapeset, tapenum, (void *) &tuplen,
							sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "tuplesort: unexpected end of data");
	return (void *) tuple;
}


/*
 * Routines specialized for IndexTuple case
 *
 * NOTE: actually, these are specialized for the btree case; it's not
 * clear whether you could use them for a non-btree index.  Possibly
 * you'd need to make another set of routines if you needed to sort
 * according to another kind of index.
 */

static int
comparetup_index(Tuplesortstate *state, const void *a, const void *b)
{
	IndexTuple	ltup = (IndexTuple) a;
	IndexTuple	rtup = (IndexTuple) b;
	TupleDesc	itdesc = state->indexRel->rd_att;
	bool		equal_isnull = false;
	Datum		lattr,
				rattr;
	bool		isnull1,
				isnull2;
	int			i;

	for (i = 0; i < itdesc->natts; i++)
	{
		lattr = index_getattr(ltup, i + 1, itdesc, &isnull1);
		rattr = index_getattr(rtup, i + 1, itdesc, &isnull2);

		if (isnull1)
		{
			if (!isnull2)
				return 1;		/* NULL sorts after non-NULL */
			equal_isnull = true;
			continue;
		}
		else if (isnull2)
			return -1;

		if (_bt_invokestrat(state->indexRel, i + 1,
							BTGreaterStrategyNumber,
							lattr, rattr))
			return 1;
		if (_bt_invokestrat(state->indexRel, i + 1,
							BTGreaterStrategyNumber,
							rattr, lattr))
			return -1;
	}

	/*
	 * If btree has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field).
	 *
	 * It is sufficient to make the test here, because if two tuples are
	 * equal they *must* get compared at some stage of the sort --- otherwise
	 * the sort algorithm wouldn't have checked whether one must appear
	 * before the other.
	 */
	if (state->enforceUnique && !equal_isnull)
		elog(ERROR, "Cannot create unique index. Table contains non-unique values");

	return 0;
}

static void *
copytup_index(Tuplesortstate *state, void *tup)
{
	IndexTuple		tuple = (IndexTuple) tup;
	unsigned int	tuplen = IndexTupleSize(tuple);
	IndexTuple		newtuple;

	USEMEM(state, tuplen);
	newtuple = (IndexTuple) palloc(tuplen);
	memcpy(newtuple, tuple, tuplen);

	return (void *) newtuple;
}

static void
writetup_index(Tuplesortstate *state, int tapenum, void *tup)
{
	IndexTuple		tuple = (IndexTuple) tup;
	unsigned int	tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void*) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void*) tuple, IndexTupleSize(tuple));
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void*) &tuplen, sizeof(tuplen));

	FREEMEM(state, IndexTupleSize(tuple));
	pfree(tuple);
}

static void *
readtup_index(Tuplesortstate *state, int tapenum, unsigned int len)
{
	unsigned int	tuplen = len - sizeof(unsigned int);
	IndexTuple		tuple = (IndexTuple) palloc(tuplen);

	USEMEM(state, tuplen);
	if (LogicalTapeRead(state->tapeset, tapenum, (void *) tuple,
						tuplen) != tuplen)
		elog(ERROR, "tuplesort: unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (LogicalTapeRead(state->tapeset, tapenum, (void *) &tuplen,
							sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "tuplesort: unexpected end of data");
	return (void *) tuple;
}
