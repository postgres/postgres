/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().	Large
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
 * selection data structure (Algorithm 5.4.1R), because it uses a fixed
 * number of records in memory at all times.  Since we are dealing with
 * tuples that may vary considerably in size, we want to be able to vary
 * the number of records kept in memory to ensure full utilization of the
 * allowed sort memory space.  So, we keep the tuples in a variable-size
 * heap, with the next record to go out at the top of the heap.  Like
 * Algorithm 5.4.1R, each record is stored with the run number that it
 * must go into, and we use (run number, key) as the ordering key for the
 * heap.  When the run number at the top of the heap changes, we know that
 * no more records of the prior run are left in the heap.
 *
 * The (approximate) amount of memory allowed for any one sort operation
 * is given in kilobytes by the external variable SortMem.	Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded SortMem.  If we reach the end of the input without
 * exceeding SortMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * SortMem, we construct a heap using Algorithm H and begin to emit tuples
 * into sorted runs in temporary tapes, emitting just enough tuples at each
 * step to get back within the SortMem limit.  Whenever the run number at
 * the top of the heap changes, we begin a new run with a new output tape
 * (selected per Algorithm D).	After the end of the input is reached,
 * we dump out remaining tuples in memory into a final run (or two),
 * then merge the runs using Algorithm D.
 *
 * When merging runs, we use a heap containing just the frontmost tuple from
 * each source run; we repeatedly output the smallest tuple and insert the
 * next tuple from its source tape (if any).  When the heap empties, the merge
 * is complete.  The basic merge algorithm thus needs very little memory ---
 * only M tuples for an M-way merge, and M is at most six in the present code.
 * However, we can still make good use of our full SortMem allocation by
 * pre-reading additional tuples from each source tape.  Without prereading,
 * our access pattern to the temporary file would be very erratic; on average
 * we'd read one block from each of M source tapes during the same time that
 * we're writing M blocks to the output tape, so there is no sequentiality of
 * access at all, defeating the read-ahead methods used by most Unix kernels.
 * Worse, the output tape gets written into a very random sequence of blocks
 * of the temp file, ensuring that things will be even worse when it comes
 * time to read that tape.	A straightforward merge pass thus ends up doing a
 * lot of waiting for disk seeks.  We can improve matters by prereading from
 * each source tape sequentially, loading about SortMem/M bytes from each tape
 * in turn.  Then we run the merge algorithm, writing but not reading until
 * one of the preloaded tuple series runs out.	Then we switch back to preread
 * mode, fill memory again, and repeat.  This approach helps to localize both
 * read and write accesses.
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.	When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
 * to one run per logical tape.  The final merge is then performed
 * on-the-fly as the caller repeatedly calls tuplesort_gettuple; this
 * saves one cycle of writing all the data out to disk and reading it in.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/tuplesort.c,v 1.37 2003/08/17 19:58:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_operator.h"
#include "miscadmin.h"
#include "utils/catcache.h"
#include "utils/datum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"


/*
 * Possible states of a Tuplesort object.  These denote the states that
 * persist between calls of Tuplesort routines.
 */
typedef enum
{
	TSS_INITIAL,				/* Loading tuples; still within memory
								 * limit */
	TSS_BUILDRUNS,				/* Loading tuples; writing to tape */
	TSS_SORTEDINMEM,			/* Sort completed entirely in memory */
	TSS_SORTEDONTAPE,			/* Sort completed, final run is on tape */
	TSS_FINALMERGE				/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * We use a seven-tape polyphase merge, which is the "sweet spot" on the
 * tapes-to-passes curve according to Knuth's figure 70 (section 5.4.2).
 */
#define MAXTAPES		7		/* Knuth's T */
#define TAPERANGE		(MAXTAPES-1)	/* Knuth's P */

/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate
{
	TupSortStatus status;		/* enumerated value as shown above */
	bool		randomAccess;	/* did caller request random access? */
	long		availMem;		/* remaining memory available, in bytes */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp
								 * file */

	/*
	 * These function pointers decouple the routines that must know what
	 * kind of tuple we are sorting from the routines that don't need to
	 * know it. They are set up by the tuplesort_begin_xxx routines.
	 *
	 * Function to compare two tuples; result is per qsort() convention, ie:
	 *
	 * <0, 0, >0 according as a<b, a=b, a>b.
	 */
	int			(*comparetup) (Tuplesortstate *state, const void *a, const void *b);

	/*
	 * Function to copy a supplied input tuple into palloc'd space. (NB:
	 * we assume that a single pfree() is enough to release the tuple
	 * later, so the representation must be "flat" in one palloc chunk.)
	 * state->availMem must be decreased by the amount of space used.
	 */
	void	   *(*copytup) (Tuplesortstate *state, void *tup);

	/*
	 * Function to write a stored tuple onto tape.	The representation of
	 * the tuple on tape need not be the same as it is in memory;
	 * requirements on the tape representation are given below.  After
	 * writing the tuple, pfree() it, and increase state->availMem by the
	 * amount of memory space thereby released.
	 */
	void		(*writetup) (Tuplesortstate *state, int tapenum, void *tup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len'
	 * is the already-read length of the stored tuple.	Create and return
	 * a palloc'd copy, and decrease state->availMem by the amount of
	 * memory space consumed.
	 */
	void	   *(*readtup) (Tuplesortstate *state, int tapenum, unsigned int len);

	/*
	 * This array holds pointers to tuples in sort memory.	If we are in
	 * state INITIAL, the tuples are in no particular order; if we are in
	 * state SORTEDINMEM, the tuples are in final sorted order; in states
	 * BUILDRUNS and FINALMERGE, the tuples are organized in "heap" order
	 * per Algorithm H.  (Note that memtupcount only counts the tuples
	 * that are part of the heap --- during merge passes, memtuples[]
	 * entries beyond TAPERANGE are never in the heap and are used to hold
	 * pre-read tuples.)  In state SORTEDONTAPE, the array is not used.
	 */
	void	  **memtuples;		/* array of pointers to palloc'd tuples */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */

	/*
	 * While building initial runs, this array holds the run number for
	 * each tuple in memtuples[].  During merge passes, we re-use it to
	 * hold the input tape number that each tuple in the heap was read
	 * from, or to hold the index of the next tuple pre-read from the same
	 * tape in the case of pre-read entries.  This array is never
	 * allocated unless we need to use tapes.  Whenever it is allocated,
	 * it has the same length as memtuples[].
	 */
	int		   *memtupindex;	/* index value associated with
								 * memtuples[i] */

	/*
	 * While building initial runs, this is the current output run number
	 * (starting at 0).  Afterwards, it is the number of initial runs we
	 * made.
	 */
	int			currentRun;

	/*
	 * These variables are only used during merge passes.  mergeactive[i]
	 * is true if we are reading an input run from (actual) tape number i
	 * and have not yet exhausted that run.  mergenext[i] is the memtuples
	 * index of the next pre-read tuple (next to be loaded into the heap)
	 * for tape i, or 0 if we are out of pre-read tuples.  mergelast[i]
	 * similarly points to the last pre-read tuple from each tape.
	 * mergeavailmem[i] is the amount of unused space allocated for tape
	 * i. mergefreelist and mergefirstfree keep track of unused locations
	 * in the memtuples[] array.  memtupindex[] links together pre-read
	 * tuples for each tape as well as recycled locations in
	 * mergefreelist. It is OK to use 0 as a null link in these lists,
	 * because memtuples[0] is part of the merge heap and is never a
	 * pre-read tuple.
	 */
	bool		mergeactive[MAXTAPES];	/* Active input run source? */
	int			mergenext[MAXTAPES];	/* first preread tuple for each
										 * source */
	int			mergelast[MAXTAPES];	/* last preread tuple for each
										 * source */
	long		mergeavailmem[MAXTAPES];		/* availMem for prereading
												 * tapes */
	long		spacePerTape;	/* actual per-tape target usage */
	int			mergefreelist;	/* head of freelist of recycled slots */
	int			mergefirstfree; /* first slot never used in this merge */

	/*
	 * Variables for Algorithm D.  Note that destTape is a "logical" tape
	 * number, ie, an index into the tp_xxx[] arrays.  Be careful to keep
	 * "logical" and "actual" tape numbers straight!
	 */
	int			Level;			/* Knuth's l */
	int			destTape;		/* current output tape (Knuth's j, less 1) */
	int			tp_fib[MAXTAPES];		/* Target Fibonacci run counts
										 * (A[]) */
	int			tp_runs[MAXTAPES];		/* # of real runs on each tape */
	int			tp_dummy[MAXTAPES];		/* # of dummy runs for each tape
										 * (D[]) */
	int			tp_tapenum[MAXTAPES];	/* Actual tape numbers (TAPE[]) */

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
	int			markpos_offset; /* saved "current", or offset in tape
								 * block */
	bool		markpos_eof;	/* saved "eof_reached" */

	/*
	 * These variables are specific to the HeapTuple case; they are set by
	 * tuplesort_begin_heap and used only by the HeapTuple routines.
	 */
	TupleDesc	tupDesc;
	int			nKeys;
	ScanKey		scanKeys;
	SortFunctionKind *sortFnKinds;

	/*
	 * These variables are specific to the IndexTuple case; they are set
	 * by tuplesort_begin_index and used only by the IndexTuple routines.
	 */
	Relation	indexRel;
	ScanKey		indexScanKey;
	bool		enforceUnique;	/* complain if we find duplicate tuples */

	/*
	 * These variables are specific to the Datum case; they are set by
	 * tuplesort_begin_datum and used only by the DatumTuple routines.
	 */
	Oid			datumType;
	Oid			sortOperator;
	FmgrInfo	sortOpFn;		/* cached lookup data for sortOperator */
	SortFunctionKind sortFnKind;
	/* we need typelen and byval in order to know how to copy the Datums. */
	int			datumTypeLen;
	bool		datumTypeByVal;
};

#define COMPARETUP(state,a,b)	((*(state)->comparetup) (state, a, b))
#define COPYTUP(state,tup)	((*(state)->copytup) (state, tup))
#define WRITETUP(state,tape,tup)	((*(state)->writetup) (state, tape, tup))
#define READTUP(state,tape,len) ((*(state)->readtup) (state, tape, len))
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
 * more than the stored length value.  This allows read-backwards.	When
 * randomAccess is not true, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplesortstate record, if needed.	They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the SortMem limit, plus
 * the space used by the variable-size arrays memtuples and memtupindex.
 * Fixed-size space (primarily the LogicalTapeSet I/O buffers) is not
 * counted.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.
 *
 *--------------------
 */

/*
 * For sorting single Datums, we build "pseudo tuples" that just carry
 * the datum's value and null flag.  For pass-by-reference data types,
 * the actual data value appears after the DatumTupleHeader (MAXALIGNed,
 * of course), and the value field in the header is just a pointer to it.
 */

typedef struct
{
	Datum		val;
	bool		isNull;
} DatumTuple;


static Tuplesortstate *tuplesort_begin_common(bool randomAccess);
static void puttuple_common(Tuplesortstate *state, void *tuple);
static void inittapes(Tuplesortstate *state);
static void selectnewtape(Tuplesortstate *state);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static void mergepreread(Tuplesortstate *state);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void tuplesort_heap_insert(Tuplesortstate *state, void *tuple,
					  int tupleindex, bool checkIndex);
static void tuplesort_heap_siftup(Tuplesortstate *state, bool checkIndex);
static unsigned int getlen(Tuplesortstate *state, int tapenum, bool eofOK);
static void markrunend(Tuplesortstate *state, int tapenum);
static int	qsort_comparetup(const void *a, const void *b);
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
static int comparetup_datum(Tuplesortstate *state,
				 const void *a, const void *b);
static void *copytup_datum(Tuplesortstate *state, void *tup);
static void writetup_datum(Tuplesortstate *state, int tapenum, void *tup);
static void *readtup_datum(Tuplesortstate *state, int tapenum,
			  unsigned int len);

/*
 * Since qsort(3) will not pass any context info to qsort_comparetup(),
 * we have to use this ugly static variable.  It is set to point to the
 * active Tuplesortstate object just before calling qsort.	It should
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
 * have been supplied.	After performsort, retrieve the tuples in sorted
 * order by calling tuplesort_gettuple until it returns NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * For Datum sorts, putdatum/getdatum are used instead of puttuple/gettuple.
 * Call tuplesort_end to terminate the operation and release memory/disk space.
 */

static Tuplesortstate *
tuplesort_begin_common(bool randomAccess)
{
	Tuplesortstate *state;

	state = (Tuplesortstate *) palloc0(sizeof(Tuplesortstate));

	state->status = TSS_INITIAL;
	state->randomAccess = randomAccess;
	state->availMem = SortMem * 1024L;
	state->tapeset = NULL;

	state->memtupcount = 0;
	state->memtupsize = 1024;	/* initial guess */
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	state->memtupindex = NULL;	/* until and unless needed */

	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	state->currentRun = 0;

	/* Algorithm D variables will be initialized by inittapes, if needed */

	state->result_tape = -1;	/* flag that result tape has not been
								 * formed */

	return state;
}

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys,
					 Oid *sortOperators, AttrNumber *attNums,
					 bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(randomAccess);
	int			i;

	AssertArg(nkeys > 0);

	state->comparetup = comparetup_heap;
	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	state->tupDesc = tupDesc;
	state->nKeys = nkeys;
	state->scanKeys = (ScanKey) palloc0(nkeys * sizeof(ScanKeyData));
	state->sortFnKinds = (SortFunctionKind *)
		palloc0(nkeys * sizeof(SortFunctionKind));

	for (i = 0; i < nkeys; i++)
	{
		RegProcedure sortFunction;

		AssertArg(sortOperators[i] != 0);
		AssertArg(attNums[i] != 0);

		/* select a function that implements the sort operator */
		SelectSortFunction(sortOperators[i], &sortFunction,
						   &state->sortFnKinds[i]);

		ScanKeyEntryInitialize(&state->scanKeys[i],
							   0x0,
							   attNums[i],
							   sortFunction,
							   (Datum) 0);
	}

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
	/* see comments below about btree dependence of this code... */
	state->indexScanKey = _bt_mkscankey_nodata(indexRel);
	state->enforceUnique = enforceUnique;

	return state;
}

Tuplesortstate *
tuplesort_begin_datum(Oid datumType,
					  Oid sortOperator,
					  bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(randomAccess);
	RegProcedure sortFunction;
	int16		typlen;
	bool		typbyval;

	state->comparetup = comparetup_datum;
	state->copytup = copytup_datum;
	state->writetup = writetup_datum;
	state->readtup = readtup_datum;

	state->datumType = datumType;
	state->sortOperator = sortOperator;

	/* select a function that implements the sort operator */
	SelectSortFunction(sortOperator, &sortFunction, &state->sortFnKind);
	/* and look up the function */
	fmgr_info(sortFunction, &state->sortOpFn);

	/* lookup necessary attributes of the datum type */
	get_typlenbyval(datumType, &typlen, &typbyval);
	state->datumTypeLen = typlen;
	state->datumTypeByVal = typbyval;

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
	int			i;

	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);
	if (state->memtuples)
	{
		for (i = 0; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	if (state->memtupindex)
		pfree(state->memtupindex);

	/*
	 * this stuff might better belong in a variant-specific shutdown
	 * routine
	 */
	if (state->scanKeys)
		pfree(state->scanKeys);
	if (state->sortFnKinds)
		pfree(state->sortFnKinds);

	pfree(state);
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
	 * Then call the code shared with the Datum case.
	 */
	tuple = COPYTUP(state, tuple);

	puttuple_common(state, tuple);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
void
tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull)
{
	DatumTuple *tuple;

	/*
	 * Build pseudo-tuple carrying the datum, and decrease availMem.
	 */
	if (isNull || state->datumTypeByVal)
	{
		tuple = (DatumTuple *) palloc(sizeof(DatumTuple));
		tuple->val = val;
		tuple->isNull = isNull;
	}
	else
	{
		Size		datalen;
		Size		tuplelen;
		char	   *newVal;

		datalen = datumGetSize(val, false, state->datumTypeLen);
		tuplelen = datalen + MAXALIGN(sizeof(DatumTuple));
		tuple = (DatumTuple *) palloc(tuplelen);
		newVal = ((char *) tuple) + MAXALIGN(sizeof(DatumTuple));
		memcpy(newVal, DatumGetPointer(val), datalen);
		tuple->val = PointerGetDatum(newVal);
		tuple->isNull = false;
	}

	USEMEM(state, GetMemoryChunkSpace(tuple));

	puttuple_common(state, (void *) tuple);
}

/*
 * Shared code for tuple and datum cases.
 */
static void
puttuple_common(Tuplesortstate *state, void *tuple)
{
	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * Save the copied tuple into the unsorted array.
			 */
			if (state->memtupcount >= state->memtupsize)
			{
				/* Grow the unsorted array as needed. */
				FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
				state->memtupsize *= 2;
				state->memtuples = (void **)
					repalloc(state->memtuples,
							 state->memtupsize * sizeof(void *));
				USEMEM(state, GetMemoryChunkSpace(state->memtuples));
			}
			state->memtuples[state->memtupcount++] = tuple;

			/*
			 * Done if we still fit in available memory.
			 */
			if (!LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.
			 */
			inittapes(state);

			/*
			 * Dump tuples until we are back under the limit.
			 */
			dumptuples(state, false);
			break;
		case TSS_BUILDRUNS:

			/*
			 * Insert the copied tuple into the heap, with run number
			 * currentRun if it can go into the current run, else run
			 * number currentRun+1.  The tuple can go into the current run
			 * if it is >= the first not-yet-output tuple.	(Actually, it
			 * could go into the current run if it is >= the most recently
			 * output tuple ... but that would require keeping around the
			 * tuple we last output, and it's simplest to let writetup
			 * free each tuple as soon as it's written.)
			 *
			 * Note there will always be at least one tuple in the heap at
			 * this point; see dumptuples.
			 */
			Assert(state->memtupcount > 0);
			if (COMPARETUP(state, tuple, state->memtuples[0]) >= 0)
				tuplesort_heap_insert(state, tuple, state->currentRun, true);
			else
				tuplesort_heap_insert(state, tuple, state->currentRun + 1, true);

			/*
			 * If we are over the memory limit, dump tuples till we're
			 * under.
			 */
			dumptuples(state, false);
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
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
			 * Finish tape-based sort.	First, flush all tuples remaining
			 * in memory out to tape; then merge until we have a single
			 * remaining run (or, if !randomAccess, one run per tape).
			 * Note that mergeruns sets the correct state->status.
			 */
			dumptuples(state, true);
			mergeruns(state);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
void *
tuplesort_gettuple(Tuplesortstate *state, bool forward,
				   bool *should_free)
{
	unsigned int tuplen;
	void	   *tup;

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
				 * if all tuples are fetched already then we return last
				 * tuple, else - tuple before last returned.
				 */
				if (state->eof_reached)
					state->eof_reached = false;
				else
				{
					state->current--;	/* last returned tuple */
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

			/*
			 * Backward.
			 *
			 * if all tuples are fetched already then we return last tuple,
			 * else - tuple before last returned.
			 */
			if (state->eof_reached)
			{
				/*
				 * Seek position is pointing just past the zero tuplen at
				 * the end of file; back up to fetch last tuple's ending
				 * length word.  If seek fails we must have a completely
				 * empty file.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  2 * sizeof(unsigned int)))
					return NULL;
				state->eof_reached = false;
			}
			else
			{
				/*
				 * Back up and fetch previously-returned tuple's ending
				 * length word.  If seek fails, assume we are at start of
				 * file.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  sizeof(unsigned int)))
					return NULL;
				tuplen = getlen(state, state->result_tape, false);

				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
									  tuplen + 2 * sizeof(unsigned int)))
				{
					/*
					 * If that fails, presumably the prev tuple is the
					 * first in the file.  Back up so that it becomes next
					 * to read in forward direction (not obviously right,
					 * but that is what in-memory case does).
					 */
					if (!LogicalTapeBackspace(state->tapeset,
											  state->result_tape,
										  tuplen + sizeof(unsigned int)))
						elog(ERROR, "bogus tuple length in backward scan");
					return NULL;
				}
			}

			tuplen = getlen(state, state->result_tape, false);

			/*
			 * Now we have the length of the prior tuple, back up and read
			 * it. Note: READTUP expects we are positioned after the
			 * initial length word of the tuple, so back up to that point.
			 */
			if (!LogicalTapeBackspace(state->tapeset,
									  state->result_tape,
									  tuplen))
				elog(ERROR, "bogus tuple length in backward scan");
			tup = READTUP(state, state->result_tape, tuplen);
			return tup;

		case TSS_FINALMERGE:
			Assert(forward);
			*should_free = true;

			/*
			 * This code should match the inner loop of mergeonerun().
			 */
			if (state->memtupcount > 0)
			{
				int			srcTape = state->memtupindex[0];
				Size		tuplen;
				int			tupIndex;
				void	   *newtup;

				tup = state->memtuples[0];
				/* returned tuple is no longer counted in our memory space */
				tuplen = GetMemoryChunkSpace(tup);
				state->availMem += tuplen;
				state->mergeavailmem[srcTape] += tuplen;
				tuplesort_heap_siftup(state, false);
				if ((tupIndex = state->mergenext[srcTape]) == 0)
				{
					/*
					 * out of preloaded data on this tape, try to read
					 * more
					 */
					mergepreread(state);

					/*
					 * if still no data, we've reached end of run on this
					 * tape
					 */
					if ((tupIndex = state->mergenext[srcTape]) == 0)
						return tup;
				}
				/* pull next preread tuple from list, insert in heap */
				newtup = state->memtuples[tupIndex];
				state->mergenext[srcTape] = state->memtupindex[tupIndex];
				if (state->mergenext[srcTape] == 0)
					state->mergelast[srcTape] = 0;
				state->memtupindex[tupIndex] = state->mergefreelist;
				state->mergefreelist = tupIndex;
				tuplesort_heap_insert(state, newtup, srcTape, false);
				return tup;
			}
			return NULL;

		default:
			elog(ERROR, "invalid tuplesort state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns FALSE if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * and is now owned by the caller.
 */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull)
{
	DatumTuple *tuple;
	bool		should_free;

	tuple = (DatumTuple *) tuplesort_gettuple(state, forward, &should_free);

	if (tuple == NULL)
		return false;

	if (tuple->isNull || state->datumTypeByVal)
	{
		*val = tuple->val;
		*isNull = tuple->isNull;
	}
	else
	{
		*val = datumCopy(tuple->val, false, state->datumTypeLen);
		*isNull = false;
	}

	if (should_free)
		pfree(tuple);

	return true;
}


/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we don't have room to sort in memory.
 */
static void
inittapes(Tuplesortstate *state)
{
	int			ntuples,
				j;

	state->tapeset = LogicalTapeSetCreate(MAXTAPES);

	/*
	 * Allocate the memtupindex array, same size as memtuples.
	 */
	state->memtupindex = (int *) palloc(state->memtupsize * sizeof(int));

	USEMEM(state, GetMemoryChunkSpace(state->memtupindex));

	/*
	 * Convert the unsorted contents of memtuples[] into a heap. Each
	 * tuple is marked as belonging to run number zero.
	 *
	 * NOTE: we pass false for checkIndex since there's no point in comparing
	 * indexes in this step, even though we do intend the indexes to be
	 * part of the sort key...
	 */
	ntuples = state->memtupcount;
	state->memtupcount = 0;		/* make the heap empty */
	for (j = 0; j < ntuples; j++)
		tuplesort_heap_insert(state, state->memtuples[j], 0, false);
	Assert(state->memtupcount == ntuples);

	state->currentRun = 0;

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
	int			j;
	int			a;

	/* Step D3: advance j (destTape) */
	if (state->tp_dummy[state->destTape] < state->tp_dummy[state->destTape + 1])
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
		state->tp_dummy[j] = a + state->tp_fib[j + 1] - state->tp_fib[j];
		state->tp_fib[j] = a + state->tp_fib[j + 1];
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
	int			tapenum,
				svTape,
				svRuns,
				svDummy;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0);

	/*
	 * If we produced only one initial run (quite likely if the total data
	 * volume is between 1X and 2X SortMem), we can just use that tape as
	 * the finished output, rather than doing a useless merge.
	 */
	if (state->currentRun == 1)
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
		while (state->tp_runs[TAPERANGE - 1] || state->tp_dummy[TAPERANGE - 1])
		{
			bool		allDummy = true;
			bool		allOneRun = true;

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
			if (!state->randomAccess && allOneRun)
			{
				Assert(!allDummy);
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
				mergeonerun(state);
		}
		/* Step D6: decrease level */
		if (--state->Level == 0)
			break;
		/* rewind output tape T to use as new input */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[TAPERANGE],
						  false);
		/* rewind used-up input tape P, and prepare it for write pass */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[TAPERANGE - 1],
						  true);
		state->tp_runs[TAPERANGE - 1] = 0;

		/*
		 * reassign tape units per step D6; note we no longer care about
		 * A[]
		 */
		svTape = state->tp_tapenum[TAPERANGE];
		svDummy = state->tp_dummy[TAPERANGE];
		svRuns = state->tp_runs[TAPERANGE];
		for (tapenum = TAPERANGE; tapenum > 0; tapenum--)
		{
			state->tp_tapenum[tapenum] = state->tp_tapenum[tapenum - 1];
			state->tp_dummy[tapenum] = state->tp_dummy[tapenum - 1];
			state->tp_runs[tapenum] = state->tp_runs[tapenum - 1];
		}
		state->tp_tapenum[0] = svTape;
		state->tp_dummy[0] = svDummy;
		state->tp_runs[0] = svRuns;
	}

	/*
	 * Done.  Knuth says that the result is on TAPE[1], but since we
	 * exited the loop without performing the last iteration of step D6,
	 * we have not rearranged the tape unit assignment, and therefore the
	 * result is on TAPE[T].  We need to do it this way so that we can
	 * freeze the final output tape while rewinding it.  The last
	 * iteration of step D6 would be a waste of cycles anyway...
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
	int			destTape = state->tp_tapenum[TAPERANGE];
	int			srcTape;
	int			tupIndex;
	void	   *tup;
	long		priorAvail,
				spaceFreed;

	/*
	 * Start the merge by loading one tuple from each active source tape
	 * into the heap.  We can also decrease the input run/dummy run
	 * counts.
	 */
	beginmerge(state);

	/*
	 * Execute merge by repeatedly extracting lowest tuple in heap,
	 * writing it out, and replacing it with next tuple from same tape (if
	 * there is another one).
	 */
	while (state->memtupcount > 0)
	{
		CHECK_FOR_INTERRUPTS();
		/* write the tuple to destTape */
		priorAvail = state->availMem;
		srcTape = state->memtupindex[0];
		WRITETUP(state, destTape, state->memtuples[0]);
		/* writetup adjusted total free space, now fix per-tape space */
		spaceFreed = state->availMem - priorAvail;
		state->mergeavailmem[srcTape] += spaceFreed;
		/* compact the heap */
		tuplesort_heap_siftup(state, false);
		if ((tupIndex = state->mergenext[srcTape]) == 0)
		{
			/* out of preloaded data on this tape, try to read more */
			mergepreread(state);
			/* if still no data, we've reached end of run on this tape */
			if ((tupIndex = state->mergenext[srcTape]) == 0)
				continue;
		}
		/* pull next preread tuple from list, insert in heap */
		tup = state->memtuples[tupIndex];
		state->mergenext[srcTape] = state->memtupindex[tupIndex];
		if (state->mergenext[srcTape] == 0)
			state->mergelast[srcTape] = 0;
		state->memtupindex[tupIndex] = state->mergefreelist;
		state->mergefreelist = tupIndex;
		tuplesort_heap_insert(state, tup, srcTape, false);
	}

	/*
	 * When the heap empties, we're done.  Write an end-of-run marker on
	 * the output tape, and increment its count of real runs.
	 */
	markrunend(state, destTape);
	state->tp_runs[TAPERANGE]++;
}

/*
 * beginmerge - initialize for a merge pass
 *
 * We decrease the counts of real and dummy runs for each tape, and mark
 * which tapes contain active input runs in mergeactive[].	Then, load
 * as many tuples as we can from each active input tape, and finally
 * fill the merge heap with the first tuple from each active tape.
 */
static void
beginmerge(Tuplesortstate *state)
{
	int			activeTapes;
	int			tapenum;
	int			srcTape;

	/* Heap should be empty here */
	Assert(state->memtupcount == 0);

	/* Clear merge-pass state variables */
	memset(state->mergeactive, 0, sizeof(state->mergeactive));
	memset(state->mergenext, 0, sizeof(state->mergenext));
	memset(state->mergelast, 0, sizeof(state->mergelast));
	memset(state->mergeavailmem, 0, sizeof(state->mergeavailmem));
	state->mergefreelist = 0;	/* nothing in the freelist */
	state->mergefirstfree = MAXTAPES;	/* first slot available for
										 * preread */

	/* Adjust run counts and mark the active tapes */
	activeTapes = 0;
	for (tapenum = 0; tapenum < TAPERANGE; tapenum++)
	{
		if (state->tp_dummy[tapenum] > 0)
			state->tp_dummy[tapenum]--;
		else
		{
			Assert(state->tp_runs[tapenum] > 0);
			state->tp_runs[tapenum]--;
			srcTape = state->tp_tapenum[tapenum];
			state->mergeactive[srcTape] = true;
			activeTapes++;
		}
	}

	/*
	 * Initialize space allocation to let each active input tape have an
	 * equal share of preread space.
	 */
	Assert(activeTapes > 0);
	state->spacePerTape = state->availMem / activeTapes;
	for (srcTape = 0; srcTape < MAXTAPES; srcTape++)
	{
		if (state->mergeactive[srcTape])
			state->mergeavailmem[srcTape] = state->spacePerTape;
	}

	/*
	 * Preread as many tuples as possible (and at least one) from each
	 * active tape
	 */
	mergepreread(state);

	/* Load the merge heap with the first tuple from each input tape */
	for (srcTape = 0; srcTape < MAXTAPES; srcTape++)
	{
		int			tupIndex = state->mergenext[srcTape];
		void	   *tup;

		if (tupIndex)
		{
			tup = state->memtuples[tupIndex];
			state->mergenext[srcTape] = state->memtupindex[tupIndex];
			if (state->mergenext[srcTape] == 0)
				state->mergelast[srcTape] = 0;
			state->memtupindex[tupIndex] = state->mergefreelist;
			state->mergefreelist = tupIndex;
			tuplesort_heap_insert(state, tup, srcTape, false);
		}
	}
}

/*
 * mergepreread - load tuples from merge input tapes
 *
 * This routine exists to improve sequentiality of reads during a merge pass,
 * as explained in the header comments of this file.  Load tuples from each
 * active source tape until the tape's run is exhausted or it has used up
 * its fair share of available memory.	In any case, we guarantee that there
 * is at one preread tuple available from each unexhausted input tape.
 */
static void
mergepreread(Tuplesortstate *state)
{
	int			srcTape;
	unsigned int tuplen;
	void	   *tup;
	int			tupIndex;
	long		priorAvail,
				spaceUsed;

	for (srcTape = 0; srcTape < MAXTAPES; srcTape++)
	{
		if (!state->mergeactive[srcTape])
			continue;

		/*
		 * Skip reading from any tape that still has at least half of its
		 * target memory filled with tuples (threshold fraction may need
		 * adjustment?).  This avoids reading just a few tuples when the
		 * incoming runs are not being consumed evenly.
		 */
		if (state->mergenext[srcTape] != 0 &&
			state->mergeavailmem[srcTape] <= state->spacePerTape / 2)
			continue;

		/*
		 * Read tuples from this tape until it has used up its free
		 * memory, but ensure that we have at least one.
		 */
		priorAvail = state->availMem;
		state->availMem = state->mergeavailmem[srcTape];
		while (!LACKMEM(state) || state->mergenext[srcTape] == 0)
		{
			/* read next tuple, if any */
			if ((tuplen = getlen(state, srcTape, true)) == 0)
			{
				state->mergeactive[srcTape] = false;
				break;
			}
			tup = READTUP(state, srcTape, tuplen);
			/* find or make a free slot in memtuples[] for it */
			tupIndex = state->mergefreelist;
			if (tupIndex)
				state->mergefreelist = state->memtupindex[tupIndex];
			else
			{
				tupIndex = state->mergefirstfree++;
				/* Might need to enlarge arrays! */
				if (tupIndex >= state->memtupsize)
				{
					FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
					FREEMEM(state, GetMemoryChunkSpace(state->memtupindex));
					state->memtupsize *= 2;
					state->memtuples = (void **)
						repalloc(state->memtuples,
								 state->memtupsize * sizeof(void *));
					state->memtupindex = (int *)
						repalloc(state->memtupindex,
								 state->memtupsize * sizeof(int));
					USEMEM(state, GetMemoryChunkSpace(state->memtuples));
					USEMEM(state, GetMemoryChunkSpace(state->memtupindex));
				}
			}
			/* store tuple, append to list for its tape */
			state->memtuples[tupIndex] = tup;
			state->memtupindex[tupIndex] = 0;
			if (state->mergelast[srcTape])
				state->memtupindex[state->mergelast[srcTape]] = tupIndex;
			else
				state->mergenext[srcTape] = tupIndex;
			state->mergelast[srcTape] = tupIndex;
		}
		/* update per-tape and global availmem counts */
		spaceUsed = state->mergeavailmem[srcTape] - state->availMem;
		state->mergeavailmem[srcTape] = state->availMem;
		state->availMem = priorAvail - spaceUsed;
	}
}

/*
 * dumptuples - remove tuples from heap and write to tape
 *
 * This is used during initial-run building, but not during merging.
 *
 * When alltuples = false, dump only enough tuples to get under the
 * availMem limit (and leave at least one tuple in the heap in any case,
 * since puttuple assumes it always has a tuple to compare to).
 *
 * When alltuples = true, dump everything currently in memory.
 * (This case is only used at end of input data.)
 *
 * If we empty the heap, close out the current run and return (this should
 * only happen at end of input data).  If we see that the tuple run number
 * at the top of the heap has changed, start a new run.
 */
static void
dumptuples(Tuplesortstate *state, bool alltuples)
{
	while (alltuples ||
		   (LACKMEM(state) && state->memtupcount > 1))
	{
		/*
		 * Dump the heap's frontmost entry, and sift up to remove it from
		 * the heap.
		 */
		Assert(state->memtupcount > 0);
		WRITETUP(state, state->tp_tapenum[state->destTape],
				 state->memtuples[0]);
		tuplesort_heap_siftup(state, true);

		/*
		 * If the heap is empty *or* top run number has changed, we've
		 * finished the current run.
		 */
		if (state->memtupcount == 0 ||
			state->currentRun != state->memtupindex[0])
		{
			markrunend(state, state->tp_tapenum[state->destTape]);
			state->currentRun++;
			state->tp_runs[state->destTape]++;
			state->tp_dummy[state->destTape]--; /* per Alg D step D2 */

			/*
			 * Done if heap is empty, else prepare for new run.
			 */
			if (state->memtupcount == 0)
				break;
			Assert(state->currentRun == state->memtupindex[0]);
			selectnewtape(state);
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
			elog(ERROR, "invalid tuplesort state");
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
							&state->markpos_block,
							&state->markpos_offset);
			state->markpos_eof = state->eof_reached;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
}

/*
 * tuplesort_restorepos - restores current position in merged sort file to
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
			if (!LogicalTapeSeek(state->tapeset,
								 state->result_tape,
								 state->markpos_block,
								 state->markpos_offset))
				elog(ERROR, "tuplesort_restorepos failed");
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
}


/*
 * Heap manipulation routines, per Knuth's Algorithm 5.2.3H.
 *
 * The heap lives in state->memtuples[], with parallel data storage
 * for indexes in state->memtupindex[].  If checkIndex is true, use
 * the tuple index as the front of the sort key; otherwise, no.
 */

#define HEAPCOMPARE(tup1,index1,tup2,index2) \
	(checkIndex && (index1 != index2) ? index1 - index2 : \
	 COMPARETUP(state, tup1, tup2))

/*
 * Insert a new tuple into an empty or existing heap, maintaining the
 * heap invariant.
 */
static void
tuplesort_heap_insert(Tuplesortstate *state, void *tuple,
					  int tupleindex, bool checkIndex)
{
	void	  **memtuples;
	int		   *memtupindex;
	int			j;

	/*
	 * Make sure memtuples[] can handle another entry.
	 */
	if (state->memtupcount >= state->memtupsize)
	{
		FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
		FREEMEM(state, GetMemoryChunkSpace(state->memtupindex));
		state->memtupsize *= 2;
		state->memtuples = (void **)
			repalloc(state->memtuples,
					 state->memtupsize * sizeof(void *));
		state->memtupindex = (int *)
			repalloc(state->memtupindex,
					 state->memtupsize * sizeof(int));
		USEMEM(state, GetMemoryChunkSpace(state->memtuples));
		USEMEM(state, GetMemoryChunkSpace(state->memtupindex));
	}
	memtuples = state->memtuples;
	memtupindex = state->memtupindex;

	/*
	 * Sift-up the new entry, per Knuth 5.2.3 exercise 16. Note that Knuth
	 * is using 1-based array indexes, not 0-based.
	 */
	j = state->memtupcount++;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (HEAPCOMPARE(tuple, tupleindex,
						memtuples[i], memtupindex[i]) >= 0)
			break;
		memtuples[j] = memtuples[i];
		memtupindex[j] = memtupindex[i];
		j = i;
	}
	memtuples[j] = tuple;
	memtupindex[j] = tupleindex;
}

/*
 * The tuple at state->memtuples[0] has been removed from the heap.
 * Decrement memtupcount, and sift up to maintain the heap invariant.
 */
static void
tuplesort_heap_siftup(Tuplesortstate *state, bool checkIndex)
{
	void	  **memtuples = state->memtuples;
	int		   *memtupindex = state->memtupindex;
	void	   *tuple;
	int			tupindex,
				i,
				n;

	if (--state->memtupcount <= 0)
		return;
	n = state->memtupcount;
	tuple = memtuples[n];		/* tuple that must be reinserted */
	tupindex = memtupindex[n];
	i = 0;						/* i is where the "hole" is */
	for (;;)
	{
		int			j = 2 * i + 1;

		if (j >= n)
			break;
		if (j + 1 < n &&
			HEAPCOMPARE(memtuples[j], memtupindex[j],
						memtuples[j + 1], memtupindex[j + 1]) > 0)
			j++;
		if (HEAPCOMPARE(tuple, tupindex,
						memtuples[j], memtupindex[j]) <= 0)
			break;
		memtuples[i] = memtuples[j];
		memtupindex[i] = memtupindex[j];
		i = j;
	}
	memtuples[i] = tuple;
	memtupindex[i] = tupindex;
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplesortstate *state, int tapenum, bool eofOK)
{
	unsigned int len;

	if (LogicalTapeRead(state->tapeset, tapenum, (void *) &len,
						sizeof(len)) != sizeof(len))
		elog(ERROR, "unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "unexpected end of data");
	return len;
}

static void
markrunend(Tuplesortstate *state, int tapenum)
{
	unsigned int len = 0;

	LogicalTapeWrite(state->tapeset, tapenum, (void *) &len, sizeof(len));
}


/*
 * qsort interface
 */

static int
qsort_comparetup(const void *a, const void *b)
{
	/* The passed pointers are pointers to void * ... */

	return COMPARETUP(qsort_tuplesortstate, *(void **) a, *(void **) b);
}


/*
 * This routine selects an appropriate sorting function to implement
 * a sort operator as efficiently as possible.	The straightforward
 * method is to use the operator's implementation proc --- ie, "<"
 * comparison.	However, that way often requires two calls of the function
 * per comparison.	If we can find a btree three-way comparator function
 * associated with the operator, we can use it to do the comparisons
 * more efficiently.  We also support the possibility that the operator
 * is ">" (descending sort), in which case we have to reverse the output
 * of the btree comparator.
 *
 * Possibly this should live somewhere else (backend/catalog/, maybe?).
 */
void
SelectSortFunction(Oid sortOperator,
				   RegProcedure *sortFunction,
				   SortFunctionKind *kind)
{
	CatCList   *catlist;
	int			i;
	HeapTuple	tuple;
	Form_pg_operator optup;
	Oid			opclass = InvalidOid;

	/*
	 * Search pg_amop to see if the target operator is registered as the
	 * "<" or ">" operator of any btree opclass.  It's possible that it
	 * might be registered both ways (eg, if someone were to build a
	 * "reverse sort" opclass for some reason); prefer the "<" case if so.
	 * If the operator is registered the same way in multiple opclasses,
	 * assume we can use the associated comparator function from any one.
	 */
	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(sortOperator),
								 0, 0, 0);

	for (i = 0; i < catlist->n_members; i++)
	{
		Form_pg_amop aform;

		tuple = &catlist->members[i]->tuple;
		aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (!opclass_is_btree(aform->amopclaid))
			continue;
		if (aform->amopstrategy == BTLessStrategyNumber)
		{
			opclass = aform->amopclaid;
			*kind = SORTFUNC_CMP;
			break;				/* done looking */
		}
		else if (aform->amopstrategy == BTGreaterStrategyNumber)
		{
			opclass = aform->amopclaid;
			*kind = SORTFUNC_REVCMP;
			/* keep scanning in hopes of finding a BTLess entry */
		}
	}

	ReleaseSysCacheList(catlist);

	if (OidIsValid(opclass))
	{
		/* Found a suitable opclass, get its comparator support function */
		*sortFunction = get_opclass_proc(opclass, BTORDER_PROC);
		Assert(RegProcedureIsValid(*sortFunction));
		return;
	}

	/*
	 * Can't find a comparator, so use the operator as-is.  Decide whether
	 * it is forward or reverse sort by looking at its name (grotty, but
	 * this only matters for deciding which end NULLs should get sorted
	 * to).  XXX possibly better idea: see whether its selectivity function
	 * is scalargtcmp?
	 */
	tuple = SearchSysCache(OPEROID,
						   ObjectIdGetDatum(sortOperator),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", sortOperator);
	optup = (Form_pg_operator) GETSTRUCT(tuple);
	if (strcmp(NameStr(optup->oprname), ">") == 0)
		*kind = SORTFUNC_REVLT;
	else
		*kind = SORTFUNC_LT;
	*sortFunction = optup->oprcode;
	ReleaseSysCache(tuple);

	Assert(RegProcedureIsValid(*sortFunction));
}

/*
 * Inline-able copy of FunctionCall2() to save some cycles in sorting.
 */
static inline Datum
myFunctionCall2(FmgrInfo *flinfo, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	/* MemSet(&fcinfo, 0, sizeof(fcinfo)); */
	fcinfo.context = NULL;
	fcinfo.resultinfo = NULL;
	fcinfo.isnull = false;

	fcinfo.flinfo = flinfo;
	fcinfo.nargs = 2;
	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

/*
 * Apply a sort function (by now converted to fmgr lookup form)
 * and return a 3-way comparison result.  This takes care of handling
 * NULLs and sort ordering direction properly.
 */
static inline int32
inlineApplySortFunction(FmgrInfo *sortFunction, SortFunctionKind kind,
						Datum datum1, bool isNull1,
						Datum datum2, bool isNull2)
{
	switch (kind)
	{
		case SORTFUNC_LT:
			if (isNull1)
			{
				if (isNull2)
					return 0;
				return 1;		/* NULL sorts after non-NULL */
			}
			if (isNull2)
				return -1;
			if (DatumGetBool(myFunctionCall2(sortFunction, datum1, datum2)))
				return -1;		/* a < b */
			if (DatumGetBool(myFunctionCall2(sortFunction, datum2, datum1)))
				return 1;		/* a > b */
			return 0;

		case SORTFUNC_REVLT:
			/* We reverse the ordering of NULLs, but not the operator */
			if (isNull1)
			{
				if (isNull2)
					return 0;
				return -1;		/* NULL sorts before non-NULL */
			}
			if (isNull2)
				return 1;
			if (DatumGetBool(myFunctionCall2(sortFunction, datum1, datum2)))
				return -1;		/* a < b */
			if (DatumGetBool(myFunctionCall2(sortFunction, datum2, datum1)))
				return 1;		/* a > b */
			return 0;

		case SORTFUNC_CMP:
			if (isNull1)
			{
				if (isNull2)
					return 0;
				return 1;		/* NULL sorts after non-NULL */
			}
			if (isNull2)
				return -1;
			return DatumGetInt32(myFunctionCall2(sortFunction,
												 datum1, datum2));

		case SORTFUNC_REVCMP:
			if (isNull1)
			{
				if (isNull2)
					return 0;
				return -1;		/* NULL sorts before non-NULL */
			}
			if (isNull2)
				return 1;
			return -DatumGetInt32(myFunctionCall2(sortFunction,
												  datum1, datum2));

		default:
			elog(ERROR, "unrecognized SortFunctionKind: %d", (int) kind);
			return 0;			/* can't get here, but keep compiler quiet */
	}
}

/*
 * Non-inline ApplySortFunction() --- this is needed only to conform to
 * C99's brain-dead notions about how to implement inline functions...
 */
int32
ApplySortFunction(FmgrInfo *sortFunction, SortFunctionKind kind,
				  Datum datum1, bool isNull1,
				  Datum datum2, bool isNull2)
{
	return inlineApplySortFunction(sortFunction, kind,
								   datum1, isNull1,
								   datum2, isNull2);
}


/*
 * Routines specialized for HeapTuple case
 */

static int
comparetup_heap(Tuplesortstate *state, const void *a, const void *b)
{
	HeapTuple	ltup = (HeapTuple) a;
	HeapTuple	rtup = (HeapTuple) b;
	TupleDesc	tupDesc = state->tupDesc;
	int			nkey;

	for (nkey = 0; nkey < state->nKeys; nkey++)
	{
		ScanKey		scanKey = state->scanKeys + nkey;
		AttrNumber	attno = scanKey->sk_attno;
		Datum		datum1,
					datum2;
		bool		isnull1,
					isnull2;
		int32		compare;

		datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

		compare = inlineApplySortFunction(&scanKey->sk_func,
										  state->sortFnKinds[nkey],
										  datum1, isnull1,
										  datum2, isnull2);
		if (compare != 0)
		{
			/* dead code? SK_COMMUTE can't actually be set here, can it? */
			if (scanKey->sk_flags & SK_COMMUTE)
				compare = -compare;
			return compare;
		}
	}

	return 0;
}

static void *
copytup_heap(Tuplesortstate *state, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;

	tuple = heap_copytuple(tuple);
	USEMEM(state, GetMemoryChunkSpace(tuple));
	return (void *) tuple;
}

/*
 * We don't bother to write the HeapTupleData part of the tuple.
 */

static void
writetup_heap(Tuplesortstate *state, int tapenum, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;
	unsigned int tuplen;

	tuplen = tuple->t_len + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_freetuple(tuple);
}

static void *
readtup_heap(Tuplesortstate *state, int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int) + HEAPTUPLESIZE;
	HeapTuple	tuple = (HeapTuple) palloc(tuplen);

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* reconstruct the HeapTupleData portion */
	tuple->t_len = len - sizeof(unsigned int);
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_datamcxt = CurrentMemoryContext;
	tuple->t_data = (HeapTupleHeader) (((char *) tuple) + HEAPTUPLESIZE);
	/* read in the tuple proper */
	if (LogicalTapeRead(state->tapeset, tapenum, (void *) tuple->t_data,
						tuple->t_len) != tuple->t_len)
		elog(ERROR, "unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (LogicalTapeRead(state->tapeset, tapenum, (void *) &tuplen,
							sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}


/*
 * Routines specialized for IndexTuple case
 *
 * NOTE: actually, these are specialized for the btree case; it's not
 * clear whether you could use them for a non-btree index.	Possibly
 * you'd need to make another set of routines if you needed to sort
 * according to another kind of index.
 */

static int
comparetup_index(Tuplesortstate *state, const void *a, const void *b)
{
	/*
	 * This is almost the same as _bt_tuplecompare(), but we need to keep
	 * track of whether any null fields are present.
	 */
	IndexTuple	tuple1 = (IndexTuple) a;
	IndexTuple	tuple2 = (IndexTuple) b;
	Relation	rel = state->indexRel;
	int			keysz = RelationGetNumberOfAttributes(rel);
	ScanKey		scankey = state->indexScanKey;
	TupleDesc	tupDes;
	int			i;
	bool		equal_hasnull = false;

	tupDes = RelationGetDescr(rel);

	for (i = 1; i <= keysz; i++)
	{
		ScanKey		entry = &scankey[i - 1];
		Datum		datum1,
					datum2;
		bool		isnull1,
					isnull2;
		int32		compare;

		datum1 = index_getattr(tuple1, i, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, i, tupDes, &isnull2);

		/* see comments about NULLs handling in btbuild */

		/* the comparison function is always of CMP type */
		compare = inlineApplySortFunction(&entry->sk_func, SORTFUNC_CMP,
										  datum1, isnull1,
										  datum2, isnull2);

		if (compare != 0)
			return (int) compare;		/* done when we find unequal
										 * attributes */

		/* they are equal, so we only need to examine one null flag */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * If btree has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field).
	 *
	 * It is sufficient to make the test here, because if two tuples are
	 * equal they *must* get compared at some stage of the sort ---
	 * otherwise the sort algorithm wouldn't have checked whether one must
	 * appear before the other.
	 *
	 * Some rather brain-dead implementations of qsort will sometimes call
	 * the comparison routine to compare a value to itself.  (At this
	 * writing only QNX 4 is known to do such silly things.)  Don't raise
	 * a bogus error in that case.
	 */
	if (state->enforceUnique && !equal_hasnull && tuple1 != tuple2)
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index"),
				 errdetail("Table contains duplicated values.")));

	return 0;
}

static void *
copytup_index(Tuplesortstate *state, void *tup)
{
	IndexTuple	tuple = (IndexTuple) tup;
	unsigned int tuplen = IndexTupleSize(tuple);
	IndexTuple	newtuple;

	newtuple = (IndexTuple) palloc(tuplen);
	USEMEM(state, GetMemoryChunkSpace(newtuple));

	memcpy(newtuple, tuple, tuplen);

	return (void *) newtuple;
}

static void
writetup_index(Tuplesortstate *state, int tapenum, void *tup)
{
	IndexTuple	tuple = (IndexTuple) tup;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tuple, IndexTupleSize(tuple));
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	pfree(tuple);
}

static void *
readtup_index(Tuplesortstate *state, int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) palloc(tuplen);

	USEMEM(state, GetMemoryChunkSpace(tuple));
	if (LogicalTapeRead(state->tapeset, tapenum, (void *) tuple,
						tuplen) != tuplen)
		elog(ERROR, "unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (LogicalTapeRead(state->tapeset, tapenum, (void *) &tuplen,
							sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}


/*
 * Routines specialized for DatumTuple case
 */

static int
comparetup_datum(Tuplesortstate *state, const void *a, const void *b)
{
	DatumTuple *ltup = (DatumTuple *) a;
	DatumTuple *rtup = (DatumTuple *) b;

	return inlineApplySortFunction(&state->sortOpFn, state->sortFnKind,
								   ltup->val, ltup->isNull,
								   rtup->val, rtup->isNull);
}

static void *
copytup_datum(Tuplesortstate *state, void *tup)
{
	/* Not currently needed */
	elog(ERROR, "copytup_datum() should not be called");
	return NULL;
}

static void
writetup_datum(Tuplesortstate *state, int tapenum, void *tup)
{
	DatumTuple *tuple = (DatumTuple *) tup;
	unsigned int tuplen;
	unsigned int writtenlen;

	if (tuple->isNull || state->datumTypeByVal)
		tuplen = sizeof(DatumTuple);
	else
	{
		Size		datalen;

		datalen = datumGetSize(tuple->val, false, state->datumTypeLen);
		tuplen = datalen + MAXALIGN(sizeof(DatumTuple));
	}

	writtenlen = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &writtenlen, sizeof(writtenlen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tuple, tuplen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &writtenlen, sizeof(writtenlen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	pfree(tuple);
}

static void *
readtup_datum(Tuplesortstate *state, int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);
	DatumTuple *tuple = (DatumTuple *) palloc(tuplen);

	USEMEM(state, GetMemoryChunkSpace(tuple));
	if (LogicalTapeRead(state->tapeset, tapenum, (void *) tuple,
						tuplen) != tuplen)
		elog(ERROR, "unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (LogicalTapeRead(state->tapeset, tapenum, (void *) &tuplen,
							sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");

	if (!tuple->isNull && !state->datumTypeByVal)
		tuple->val = PointerGetDatum(((char *) tuple) +
									 MAXALIGN(sizeof(DatumTuple)));
	return (void *) tuple;
}
