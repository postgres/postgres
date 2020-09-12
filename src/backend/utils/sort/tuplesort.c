/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().  Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * See Knuth, volume 3, for more than you want to know about the external
 * sorting algorithm.  Historically, we divided the input into sorted runs
 * using replacement selection, in the form of a priority tree implemented
 * as a heap (essentially his Algorithm 5.2.3H), but now we always use
 * quicksort for run generation.  We merge the runs using polyphase merge,
 * Knuth's Algorithm 5.4.2D.  The logical "tapes" used by Algorithm D are
 * implemented by logtape.c, which avoids space wastage by recycling disk
 * space as soon as each block is read from its "tape".
 *
 * The approximate amount of memory allowed for any one sort operation
 * is specified in kilobytes by the caller (most pass work_mem).  Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded workMem.  If we reach the end of the input without
 * exceeding workMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * workMem, we begin to emit tuples into sorted runs in temporary tapes.
 * When tuples are dumped in batch after quicksorting, we begin a new run
 * with a new output tape (selected per Algorithm D).  After the end of the
 * input is reached, we dump out remaining tuples in memory into a final run,
 * then merge the runs using Algorithm D.
 *
 * When merging runs, we use a heap containing just the frontmost tuple from
 * each source run; we repeatedly output the smallest tuple and replace it
 * with the next tuple from its source tape (if any).  When the heap empties,
 * the merge is complete.  The basic merge algorithm thus needs very little
 * memory --- only M tuples for an M-way merge, and M is constrained to a
 * small number.  However, we can still make good use of our full workMem
 * allocation by pre-reading additional blocks from each source tape.  Without
 * prereading, our access pattern to the temporary file would be very erratic;
 * on average we'd read one block from each of M source tapes during the same
 * time that we're writing M blocks to the output tape, so there is no
 * sequentiality of access at all, defeating the read-ahead methods used by
 * most Unix kernels.  Worse, the output tape gets written into a very random
 * sequence of blocks of the temp file, ensuring that things will be even
 * worse when it comes time to read that tape.  A straightforward merge pass
 * thus ends up doing a lot of waiting for disk seeks.  We can improve matters
 * by prereading from each source tape sequentially, loading about workMem/M
 * bytes from each tape in turn, and making the sequential blocks immediately
 * available for reuse.  This approach helps to localize both read and write
 * accesses.  The pre-reading is handled by logtape.c, we just tell it how
 * much memory to use for the buffers.
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.  When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
 * to one run per logical tape.  The final merge is then performed
 * on-the-fly as the caller repeatedly calls tuplesort_getXXX; this
 * saves one cycle of writing all the data out to disk and reading it in.
 *
 * Before Postgres 8.2, we always used a seven-tape polyphase merge, on the
 * grounds that 7 is the "sweet spot" on the tapes-to-passes curve according
 * to Knuth's figure 70 (section 5.4.2).  However, Knuth is assuming that
 * tape drives are expensive beasts, and in particular that there will always
 * be many more runs than tape drives.  In our implementation a "tape drive"
 * doesn't cost much more than a few Kb of memory buffers, so we can afford
 * to have lots of them.  In particular, if we can have as many tape drives
 * as sorted runs, we can eliminate any repeated I/O at all.  In the current
 * code we determine the number of tapes M on the basis of workMem: we want
 * workMem/M to be large enough that we read a fair amount of data each time
 * we preread from a tape, so as to maintain the locality of access described
 * above.  Nonetheless, with large workMem we can have many tapes (but not
 * too many -- see the comments in tuplesort_merge_order).
 *
 * This module supports parallel sorting.  Parallel sorts involve coordination
 * among one or more worker processes, and a leader process, each with its own
 * tuplesort state.  The leader process (or, more accurately, the
 * Tuplesortstate associated with a leader process) creates a full tapeset
 * consisting of worker tapes with one run to merge; a run for every
 * worker process.  This is then merged.  Worker processes are guaranteed to
 * produce exactly one output run from their partial input.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "utils/datum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"


/* sort-type codes for sort__start probes */
#define HEAP_SORT		0
#define INDEX_SORT		1
#define DATUM_SORT		2
#define CLUSTER_SORT	3

/* Sort parallel code from state for sort__start probes */
#define PARALLEL_SORT(state)	((state)->shared == NULL ? 0 : \
								 (state)->worker >= 0 ? 1 : 2)

/*
 * Initial size of memtuples array.  We're trying to select this size so that
 * array doesn't exceed ALLOCSET_SEPARATE_THRESHOLD and so that the overhead of
 * allocation might possibly be lowered.  However, we don't consider array sizes
 * less than 1024.
 *
 */
#define INITIAL_MEMTUPSIZE Max(1024, \
	ALLOCSET_SEPARATE_THRESHOLD / sizeof(SortTuple) + 1)

/* GUC variables */
#ifdef TRACE_SORT
bool		trace_sort = false;
#endif

#ifdef DEBUG_BOUNDED_SORT
bool		optimize_bounded_sort = true;
#endif


/*
 * The objects we actually sort are SortTuple structs.  These contain
 * a pointer to the tuple proper (might be a MinimalTuple or IndexTuple),
 * which is a separate palloc chunk --- we assume it is just one chunk and
 * can be freed by a simple pfree() (except during merge, when we use a
 * simple slab allocator).  SortTuples also contain the tuple's first key
 * column in Datum/nullflag format, and a source/input tape number that
 * tracks which tape each heap element/slot belongs to during merging.
 *
 * Storing the first key column lets us save heap_getattr or index_getattr
 * calls during tuple comparisons.  We could extract and save all the key
 * columns not just the first, but this would increase code complexity and
 * overhead, and wouldn't actually save any comparison cycles in the common
 * case where the first key determines the comparison result.  Note that
 * for a pass-by-reference datatype, datum1 points into the "tuple" storage.
 *
 * There is one special case: when the sort support infrastructure provides an
 * "abbreviated key" representation, where the key is (typically) a pass by
 * value proxy for a pass by reference type.  In this case, the abbreviated key
 * is stored in datum1 in place of the actual first key column.
 *
 * When sorting single Datums, the data value is represented directly by
 * datum1/isnull1 for pass by value types (or null values).  If the datatype is
 * pass-by-reference and isnull1 is false, then "tuple" points to a separately
 * palloc'd data value, otherwise "tuple" is NULL.  The value of datum1 is then
 * either the same pointer as "tuple", or is an abbreviated key value as
 * described above.  Accordingly, "tuple" is always used in preference to
 * datum1 as the authoritative value for pass-by-reference cases.
 */
typedef struct
{
	void	   *tuple;			/* the tuple itself */
	Datum		datum1;			/* value of first key column */
	bool		isnull1;		/* is first key column NULL? */
	int			srctape;		/* source tape number */
} SortTuple;

/*
 * During merge, we use a pre-allocated set of fixed-size slots to hold
 * tuples.  To avoid palloc/pfree overhead.
 *
 * Merge doesn't require a lot of memory, so we can afford to waste some,
 * by using gratuitously-sized slots.  If a tuple is larger than 1 kB, the
 * palloc() overhead is not significant anymore.
 *
 * 'nextfree' is valid when this chunk is in the free list.  When in use, the
 * slot holds a tuple.
 */
#define SLAB_SLOT_SIZE 1024

typedef union SlabSlot
{
	union SlabSlot *nextfree;
	char		buffer[SLAB_SLOT_SIZE];
} SlabSlot;

/*
 * Possible states of a Tuplesort object.  These denote the states that
 * persist between calls of Tuplesort routines.
 */
typedef enum
{
	TSS_INITIAL,				/* Loading tuples; still within memory limit */
	TSS_BOUNDED,				/* Loading tuples into bounded-size heap */
	TSS_BUILDRUNS,				/* Loading tuples; writing to tape */
	TSS_SORTEDINMEM,			/* Sort completed entirely in memory */
	TSS_SORTEDONTAPE,			/* Sort completed, final run is on tape */
	TSS_FINALMERGE				/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * Parameters for calculation of number of tapes to use --- see inittapes()
 * and tuplesort_merge_order().
 *
 * In this calculation we assume that each tape will cost us about 1 blocks
 * worth of buffer space.  This ignores the overhead of all the other data
 * structures needed for each tape, but it's probably close enough.
 *
 * MERGE_BUFFER_SIZE is how much data we'd like to read from each input
 * tape during a preread cycle (see discussion at top of file).
 */
#define MINORDER		6		/* minimum merge order */
#define MAXORDER		500		/* maximum merge order */
#define TAPE_BUFFER_OVERHEAD		BLCKSZ
#define MERGE_BUFFER_SIZE			(BLCKSZ * 32)

typedef int (*SortTupleComparator) (const SortTuple *a, const SortTuple *b,
									Tuplesortstate *state);

/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate
{
	TupSortStatus status;		/* enumerated value as shown above */
	int			nKeys;			/* number of columns in sort key */
	bool		randomAccess;	/* did caller request random access? */
	bool		bounded;		/* did caller specify a maximum number of
								 * tuples to return? */
	bool		boundUsed;		/* true if we made use of a bounded heap */
	int			bound;			/* if bounded, the maximum number of tuples */
	bool		tuples;			/* Can SortTuple.tuple ever be set? */
	int64		availMem;		/* remaining memory available, in bytes */
	int64		allowedMem;		/* total memory allowed, in bytes */
	int			maxTapes;		/* number of tapes (Knuth's T) */
	int			tapeRange;		/* maxTapes-1 (Knuth's P) */
	int64		maxSpace;		/* maximum amount of space occupied among sort
								 * of groups, either in-memory or on-disk */
	bool		isMaxSpaceDisk; /* true when maxSpace is value for on-disk
								 * space, false when it's value for in-memory
								 * space */
	TupSortStatus maxSpaceStatus;	/* sort status when maxSpace was reached */
	MemoryContext maincontext;	/* memory context for tuple sort metadata that
								 * persists across multiple batches */
	MemoryContext sortcontext;	/* memory context holding most sort data */
	MemoryContext tuplecontext; /* sub-context of sortcontext for tuple data */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

	/*
	 * These function pointers decouple the routines that must know what kind
	 * of tuple we are sorting from the routines that don't need to know it.
	 * They are set up by the tuplesort_begin_xxx routines.
	 *
	 * Function to compare two tuples; result is per qsort() convention, ie:
	 * <0, 0, >0 according as a<b, a=b, a>b.  The API must match
	 * qsort_arg_comparator.
	 */
	SortTupleComparator comparetup;

	/*
	 * Function to copy a supplied input tuple into palloc'd space and set up
	 * its SortTuple representation (ie, set tuple/datum1/isnull1).  Also,
	 * state->availMem must be decreased by the amount of space used for the
	 * tuple copy (note the SortTuple struct itself is not counted).
	 */
	void		(*copytup) (Tuplesortstate *state, SortTuple *stup, void *tup);

	/*
	 * Function to write a stored tuple onto tape.  The representation of the
	 * tuple on tape need not be the same as it is in memory; requirements on
	 * the tape representation are given below.  Unless the slab allocator is
	 * used, after writing the tuple, pfree() the out-of-line data (not the
	 * SortTuple struct!), and increase state->availMem by the amount of
	 * memory space thereby released.
	 */
	void		(*writetup) (Tuplesortstate *state, int tapenum,
							 SortTuple *stup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len' is
	 * the already-read length of the stored tuple.  The tuple is allocated
	 * from the slab memory arena, or is palloc'd, see readtup_alloc().
	 */
	void		(*readtup) (Tuplesortstate *state, SortTuple *stup,
							int tapenum, unsigned int len);

	/*
	 * This array holds the tuples now in sort memory.  If we are in state
	 * INITIAL, the tuples are in no particular order; if we are in state
	 * SORTEDINMEM, the tuples are in final sorted order; in states BUILDRUNS
	 * and FINALMERGE, the tuples are organized in "heap" order per Algorithm
	 * H.  In state SORTEDONTAPE, the array is not used.
	 */
	SortTuple  *memtuples;		/* array of SortTuple structs */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */
	bool		growmemtuples;	/* memtuples' growth still underway? */

	/*
	 * Memory for tuples is sometimes allocated using a simple slab allocator,
	 * rather than with palloc().  Currently, we switch to slab allocation
	 * when we start merging.  Merging only needs to keep a small, fixed
	 * number of tuples in memory at any time, so we can avoid the
	 * palloc/pfree overhead by recycling a fixed number of fixed-size slots
	 * to hold the tuples.
	 *
	 * For the slab, we use one large allocation, divided into SLAB_SLOT_SIZE
	 * slots.  The allocation is sized to have one slot per tape, plus one
	 * additional slot.  We need that many slots to hold all the tuples kept
	 * in the heap during merge, plus the one we have last returned from the
	 * sort, with tuplesort_gettuple.
	 *
	 * Initially, all the slots are kept in a linked list of free slots.  When
	 * a tuple is read from a tape, it is put to the next available slot, if
	 * it fits.  If the tuple is larger than SLAB_SLOT_SIZE, it is palloc'd
	 * instead.
	 *
	 * When we're done processing a tuple, we return the slot back to the free
	 * list, or pfree() if it was palloc'd.  We know that a tuple was
	 * allocated from the slab, if its pointer value is between
	 * slabMemoryBegin and -End.
	 *
	 * When the slab allocator is used, the USEMEM/LACKMEM mechanism of
	 * tracking memory usage is not used.
	 */
	bool		slabAllocatorUsed;

	char	   *slabMemoryBegin;	/* beginning of slab memory arena */
	char	   *slabMemoryEnd;	/* end of slab memory arena */
	SlabSlot   *slabFreeHead;	/* head of free list */

	/* Buffer size to use for reading input tapes, during merge. */
	size_t		read_buffer_size;

	/*
	 * When we return a tuple to the caller in tuplesort_gettuple_XXX, that
	 * came from a tape (that is, in TSS_SORTEDONTAPE or TSS_FINALMERGE
	 * modes), we remember the tuple in 'lastReturnedTuple', so that we can
	 * recycle the memory on next gettuple call.
	 */
	void	   *lastReturnedTuple;

	/*
	 * While building initial runs, this is the current output run number.
	 * Afterwards, it is the number of initial runs we made.
	 */
	int			currentRun;

	/*
	 * Unless otherwise noted, all pointer variables below are pointers to
	 * arrays of length maxTapes, holding per-tape data.
	 */

	/*
	 * This variable is only used during merge passes.  mergeactive[i] is true
	 * if we are reading an input run from (actual) tape number i and have not
	 * yet exhausted that run.
	 */
	bool	   *mergeactive;	/* active input run source? */

	/*
	 * Variables for Algorithm D.  Note that destTape is a "logical" tape
	 * number, ie, an index into the tp_xxx[] arrays.  Be careful to keep
	 * "logical" and "actual" tape numbers straight!
	 */
	int			Level;			/* Knuth's l */
	int			destTape;		/* current output tape (Knuth's j, less 1) */
	int		   *tp_fib;			/* Target Fibonacci run counts (A[]) */
	int		   *tp_runs;		/* # of real runs on each tape */
	int		   *tp_dummy;		/* # of dummy runs for each tape (D[]) */
	int		   *tp_tapenum;		/* Actual tape numbers (TAPE[]) */
	int			activeTapes;	/* # of active input tapes in merge pass */

	/*
	 * These variables are used after completion of sorting to keep track of
	 * the next tuple to return.  (In the tape case, the tape's current read
	 * position is also critical state.)
	 */
	int			result_tape;	/* actual tape number of finished output */
	int			current;		/* array index (only used if SORTEDINMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	long		markpos_block;	/* tape block# (only used if SORTEDONTAPE) */
	int			markpos_offset; /* saved "current", or offset in tape block */
	bool		markpos_eof;	/* saved "eof_reached" */

	/*
	 * These variables are used during parallel sorting.
	 *
	 * worker is our worker identifier.  Follows the general convention that
	 * -1 value relates to a leader tuplesort, and values >= 0 worker
	 * tuplesorts. (-1 can also be a serial tuplesort.)
	 *
	 * shared is mutable shared memory state, which is used to coordinate
	 * parallel sorts.
	 *
	 * nParticipants is the number of worker Tuplesortstates known by the
	 * leader to have actually been launched, which implies that they must
	 * finish a run leader can merge.  Typically includes a worker state held
	 * by the leader process itself.  Set in the leader Tuplesortstate only.
	 */
	int			worker;
	Sharedsort *shared;
	int			nParticipants;

	/*
	 * The sortKeys variable is used by every case other than the hash index
	 * case; it is set by tuplesort_begin_xxx.  tupDesc is only used by the
	 * MinimalTuple and CLUSTER routines, though.
	 */
	TupleDesc	tupDesc;
	SortSupport sortKeys;		/* array of length nKeys */

	/*
	 * This variable is shared by the single-key MinimalTuple case and the
	 * Datum case (which both use qsort_ssup()).  Otherwise it's NULL.
	 */
	SortSupport onlyKey;

	/*
	 * Additional state for managing "abbreviated key" sortsupport routines
	 * (which currently may be used by all cases except the hash index case).
	 * Tracks the intervals at which the optimization's effectiveness is
	 * tested.
	 */
	int64		abbrevNext;		/* Tuple # at which to next check
								 * applicability */

	/*
	 * These variables are specific to the CLUSTER case; they are set by
	 * tuplesort_begin_cluster.
	 */
	IndexInfo  *indexInfo;		/* info about index being used for reference */
	EState	   *estate;			/* for evaluating index expressions */

	/*
	 * These variables are specific to the IndexTuple case; they are set by
	 * tuplesort_begin_index_xxx and used only by the IndexTuple routines.
	 */
	Relation	heapRel;		/* table the index is being built on */
	Relation	indexRel;		/* index being built */

	/* These are specific to the index_btree subcase: */
	bool		enforceUnique;	/* complain if we find duplicate tuples */

	/* These are specific to the index_hash subcase: */
	uint32		high_mask;		/* masks for sortable part of hash code */
	uint32		low_mask;
	uint32		max_buckets;

	/*
	 * These variables are specific to the Datum case; they are set by
	 * tuplesort_begin_datum and used only by the DatumTuple routines.
	 */
	Oid			datumType;
	/* we need typelen in order to know how to copy the Datums. */
	int			datumTypeLen;

	/*
	 * Resource snapshot for time of sort start.
	 */
#ifdef TRACE_SORT
	PGRUsage	ru_start;
#endif
};

/*
 * Private mutable state of tuplesort-parallel-operation.  This is allocated
 * in shared memory.
 */
struct Sharedsort
{
	/* mutex protects all fields prior to tapes */
	slock_t		mutex;

	/*
	 * currentWorker generates ordinal identifier numbers for parallel sort
	 * workers.  These start from 0, and are always gapless.
	 *
	 * Workers increment workersFinished to indicate having finished.  If this
	 * is equal to state.nParticipants within the leader, leader is ready to
	 * merge worker runs.
	 */
	int			currentWorker;
	int			workersFinished;

	/* Temporary file space */
	SharedFileSet fileset;

	/* Size of tapes flexible array */
	int			nTapes;

	/*
	 * Tapes array used by workers to report back information needed by the
	 * leader to concatenate all worker tapes into one for merging
	 */
	TapeShare	tapes[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * Is the given tuple allocated from the slab memory arena?
 */
#define IS_SLAB_SLOT(state, tuple) \
	((char *) (tuple) >= (state)->slabMemoryBegin && \
	 (char *) (tuple) < (state)->slabMemoryEnd)

/*
 * Return the given tuple to the slab memory free list, or free it
 * if it was palloc'd.
 */
#define RELEASE_SLAB_SLOT(state, tuple) \
	do { \
		SlabSlot *buf = (SlabSlot *) tuple; \
		\
		if (IS_SLAB_SLOT((state), buf)) \
		{ \
			buf->nextfree = (state)->slabFreeHead; \
			(state)->slabFreeHead = buf; \
		} else \
			pfree(buf); \
	} while(0)

#define COMPARETUP(state,a,b)	((*(state)->comparetup) (a, b, state))
#define COPYTUP(state,stup,tup) ((*(state)->copytup) (state, stup, tup))
#define WRITETUP(state,tape,stup)	((*(state)->writetup) (state, tape, stup))
#define READTUP(state,stup,tape,len) ((*(state)->readtup) (state, stup, tape, len))
#define LACKMEM(state)		((state)->availMem < 0 && !(state)->slabAllocatorUsed)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))
#define SERIAL(state)		((state)->shared == NULL)
#define WORKER(state)		((state)->shared && (state)->worker != -1)
#define LEADER(state)		((state)->shared && (state)->worker == -1)

/*
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
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the workMem limit, plus
 * the space used by the variable-size memtuples array.  Fixed-size space
 * is not counted; it's small enough to not be interesting.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.  As of 9.6, a
 * separate memory context is used for caller passed tuples.  Resetting
 * it at certain key increments significantly ameliorates fragmentation.
 * Note that this places a responsibility on copytup routines to use the
 * correct memory context for these tuples (and to not use the reset
 * context for anything whose lifetime needs to span multiple external
 * sort runs).  readtup routines use the slab allocator (they cannot use
 * the reset context because it gets deleted at the point that merging
 * begins).
 */

/* When using this macro, beware of double evaluation of len */
#define LogicalTapeReadExact(tapeset, tapenum, ptr, len) \
	do { \
		if (LogicalTapeRead(tapeset, tapenum, ptr, len) != (size_t) (len)) \
			elog(ERROR, "unexpected end of data"); \
	} while(0)


static Tuplesortstate *tuplesort_begin_common(int workMem,
											  SortCoordinate coordinate,
											  bool randomAccess);
static void tuplesort_begin_batch(Tuplesortstate *state);
static void puttuple_common(Tuplesortstate *state, SortTuple *tuple);
static bool consider_abort_common(Tuplesortstate *state);
static void inittapes(Tuplesortstate *state, bool mergeruns);
static void inittapestate(Tuplesortstate *state, int maxTapes);
static void selectnewtape(Tuplesortstate *state);
static void init_slab_allocator(Tuplesortstate *state, int numSlots);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static bool mergereadnext(Tuplesortstate *state, int srcTape, SortTuple *stup);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void make_bounded_heap(Tuplesortstate *state);
static void sort_bounded_heap(Tuplesortstate *state);
static void tuplesort_sort_memtuples(Tuplesortstate *state);
static void tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_replace_top(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_delete_top(Tuplesortstate *state);
static void reversedirection(Tuplesortstate *state);
static unsigned int getlen(Tuplesortstate *state, int tapenum, bool eofOK);
static void markrunend(Tuplesortstate *state, int tapenum);
static void *readtup_alloc(Tuplesortstate *state, Size tuplen);
static int	comparetup_heap(const SortTuple *a, const SortTuple *b,
							Tuplesortstate *state);
static void copytup_heap(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_heap(Tuplesortstate *state, int tapenum,
						  SortTuple *stup);
static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
						 int tapenum, unsigned int len);
static int	comparetup_cluster(const SortTuple *a, const SortTuple *b,
							   Tuplesortstate *state);
static void copytup_cluster(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_cluster(Tuplesortstate *state, int tapenum,
							 SortTuple *stup);
static void readtup_cluster(Tuplesortstate *state, SortTuple *stup,
							int tapenum, unsigned int len);
static int	comparetup_index_btree(const SortTuple *a, const SortTuple *b,
								   Tuplesortstate *state);
static int	comparetup_index_hash(const SortTuple *a, const SortTuple *b,
								  Tuplesortstate *state);
static void copytup_index(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_index(Tuplesortstate *state, int tapenum,
						   SortTuple *stup);
static void readtup_index(Tuplesortstate *state, SortTuple *stup,
						  int tapenum, unsigned int len);
static int	comparetup_datum(const SortTuple *a, const SortTuple *b,
							 Tuplesortstate *state);
static void copytup_datum(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_datum(Tuplesortstate *state, int tapenum,
						   SortTuple *stup);
static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
						  int tapenum, unsigned int len);
static int	worker_get_identifier(Tuplesortstate *state);
static void worker_freeze_result_tape(Tuplesortstate *state);
static void worker_nomergeruns(Tuplesortstate *state);
static void leader_takeover_tapes(Tuplesortstate *state);
static void free_sort_tuple(Tuplesortstate *state, SortTuple *stup);
static void tuplesort_free(Tuplesortstate *state);
static void tuplesort_updatemax(Tuplesortstate *state);

/*
 * Special versions of qsort just for SortTuple objects.  qsort_tuple() sorts
 * any variant of SortTuples, using the appropriate comparetup function.
 * qsort_ssup() is specialized for the case where the comparetup function
 * reduces to ApplySortComparator(), that is single-key MinimalTuple sorts
 * and Datum sorts.
 */
#include "qsort_tuple.c"


/*
 *		tuplesort_begin_xxx
 *
 * Initialize for a tuple sort operation.
 *
 * After calling tuplesort_begin, the caller should call tuplesort_putXXX
 * zero or more times, then call tuplesort_performsort when all the tuples
 * have been supplied.  After performsort, retrieve the tuples in sorted
 * order by calling tuplesort_getXXX until it returns false/NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * Call tuplesort_end to terminate the operation and release memory/disk space.
 *
 * Each variant of tuplesort_begin has a workMem parameter specifying the
 * maximum number of kilobytes of RAM to use before spilling data to disk.
 * (The normal value of this parameter is work_mem, but some callers use
 * other values.)  Each variant also has a randomAccess parameter specifying
 * whether the caller needs non-sequential access to the sort result.
 */

static Tuplesortstate *
tuplesort_begin_common(int workMem, SortCoordinate coordinate,
					   bool randomAccess)
{
	Tuplesortstate *state;
	MemoryContext maincontext;
	MemoryContext sortcontext;
	MemoryContext oldcontext;

	/* See leader_takeover_tapes() remarks on randomAccess support */
	if (coordinate && randomAccess)
		elog(ERROR, "random access disallowed under parallel sort");

	/*
	 * Memory context surviving tuplesort_reset.  This memory context holds
	 * data which is useful to keep while sorting multiple similar batches.
	 */
	maincontext = AllocSetContextCreate(CurrentMemoryContext,
										"TupleSort main",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * Create a working memory context for one sort operation.  The content of
	 * this context is deleted by tuplesort_reset.
	 */
	sortcontext = AllocSetContextCreate(maincontext,
										"TupleSort sort",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * Additionally a working memory context for tuples is setup in
	 * tuplesort_begin_batch.
	 */

	/*
	 * Make the Tuplesortstate within the per-sortstate context.  This way, we
	 * don't need a separate pfree() operation for it at shutdown.
	 */
	oldcontext = MemoryContextSwitchTo(maincontext);

	state = (Tuplesortstate *) palloc0(sizeof(Tuplesortstate));

#ifdef TRACE_SORT
	if (trace_sort)
		pg_rusage_init(&state->ru_start);
#endif

	state->randomAccess = randomAccess;
	state->tuples = true;

	/*
	 * workMem is forced to be at least 64KB, the current minimum valid value
	 * for the work_mem GUC.  This is a defense against parallel sort callers
	 * that divide out memory among many workers in a way that leaves each
	 * with very little memory.
	 */
	state->allowedMem = Max(workMem, 64) * (int64) 1024;
	state->sortcontext = sortcontext;
	state->maincontext = maincontext;

	/*
	 * Initial size of array must be more than ALLOCSET_SEPARATE_THRESHOLD;
	 * see comments in grow_memtuples().
	 */
	state->memtupsize = INITIAL_MEMTUPSIZE;
	state->memtuples = NULL;

	/*
	 * After all of the other non-parallel-related state, we setup all of the
	 * state needed for each batch.
	 */
	tuplesort_begin_batch(state);

	/*
	 * Initialize parallel-related state based on coordination information
	 * from caller
	 */
	if (!coordinate)
	{
		/* Serial sort */
		state->shared = NULL;
		state->worker = -1;
		state->nParticipants = -1;
	}
	else if (coordinate->isWorker)
	{
		/* Parallel worker produces exactly one final run from all input */
		state->shared = coordinate->sharedsort;
		state->worker = worker_get_identifier(state);
		state->nParticipants = -1;
	}
	else
	{
		/* Parallel leader state only used for final merge */
		state->shared = coordinate->sharedsort;
		state->worker = -1;
		state->nParticipants = coordinate->nParticipants;
		Assert(state->nParticipants >= 1);
	}

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 *		tuplesort_begin_batch
 *
 * Setup, or reset, all state need for processing a new set of tuples with this
 * sort state. Called both from tuplesort_begin_common (the first time sorting
 * with this sort state) and tuplesort_reset (for subsequent usages).
 */
static void
tuplesort_begin_batch(Tuplesortstate *state)
{
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(state->maincontext);

	/*
	 * Caller tuple (e.g. IndexTuple) memory context.
	 *
	 * A dedicated child context used exclusively for caller passed tuples
	 * eases memory management.  Resetting at key points reduces
	 * fragmentation. Note that the memtuples array of SortTuples is allocated
	 * in the parent context, not this context, because there is no need to
	 * free memtuples early.
	 */
	state->tuplecontext = AllocSetContextCreate(state->sortcontext,
												"Caller tuples",
												ALLOCSET_DEFAULT_SIZES);

	state->status = TSS_INITIAL;
	state->bounded = false;
	state->boundUsed = false;

	state->availMem = state->allowedMem;

	state->tapeset = NULL;

	state->memtupcount = 0;

	/*
	 * Initial size of array must be more than ALLOCSET_SEPARATE_THRESHOLD;
	 * see comments in grow_memtuples().
	 */
	state->growmemtuples = true;
	state->slabAllocatorUsed = false;
	if (state->memtuples != NULL && state->memtupsize != INITIAL_MEMTUPSIZE)
	{
		pfree(state->memtuples);
		state->memtuples = NULL;
		state->memtupsize = INITIAL_MEMTUPSIZE;
	}
	if (state->memtuples == NULL)
	{
		state->memtuples = (SortTuple *) palloc(state->memtupsize * sizeof(SortTuple));
		USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	}

	/* workMem must be large enough for the minimal memtuples array */
	if (LACKMEM(state))
		elog(ERROR, "insufficient memory allowed for sort");

	state->currentRun = 0;

	/*
	 * maxTapes, tapeRange, and Algorithm D variables will be initialized by
	 * inittapes(), if needed
	 */

	state->result_tape = -1;	/* flag that result tape has not been formed */

	MemoryContextSwitchTo(oldcontext);
}

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, AttrNumber *attNums,
					 Oid *sortOperators, Oid *sortCollations,
					 bool *nullsFirstFlags,
					 int workMem, SortCoordinate coordinate, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   randomAccess);
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(state->maincontext);

	AssertArg(nkeys > 0);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 nkeys, workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = nkeys;

	TRACE_POSTGRESQL_SORT_START(HEAP_SORT,
								false,	/* no unique check */
								nkeys,
								workMem,
								randomAccess,
								PARALLEL_SORT(state));

	state->comparetup = comparetup_heap;
	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	state->tupDesc = tupDesc;	/* assume we need not copy tupDesc */
	state->abbrevNext = 10;

	/* Prepare SortSupport data for each column */
	state->sortKeys = (SortSupport) palloc0(nkeys * sizeof(SortSupportData));

	for (i = 0; i < nkeys; i++)
	{
		SortSupport sortKey = state->sortKeys + i;

		AssertArg(attNums[i] != 0);
		AssertArg(sortOperators[i] != 0);

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = sortCollations[i];
		sortKey->ssup_nulls_first = nullsFirstFlags[i];
		sortKey->ssup_attno = attNums[i];
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0);

		PrepareSortSupportFromOrderingOp(sortOperators[i], sortKey);
	}

	/*
	 * The "onlyKey" optimization cannot be used with abbreviated keys, since
	 * tie-breaker comparisons may be required.  Typically, the optimization
	 * is only of value to pass-by-value types anyway, whereas abbreviated
	 * keys are typically only of value to pass-by-reference types.
	 */
	if (nkeys == 1 && !state->sortKeys->abbrev_converter)
		state->onlyKey = state->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_cluster(TupleDesc tupDesc,
						Relation indexRel,
						int workMem,
						SortCoordinate coordinate, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   randomAccess);
	BTScanInsert indexScanKey;
	MemoryContext oldcontext;
	int			i;

	Assert(indexRel->rd_rel->relam == BTREE_AM_OID);

	oldcontext = MemoryContextSwitchTo(state->maincontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 RelationGetNumberOfAttributes(indexRel),
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(CLUSTER_SORT,
								false,	/* no unique check */
								state->nKeys,
								workMem,
								randomAccess,
								PARALLEL_SORT(state));

	state->comparetup = comparetup_cluster;
	state->copytup = copytup_cluster;
	state->writetup = writetup_cluster;
	state->readtup = readtup_cluster;
	state->abbrevNext = 10;

	state->indexInfo = BuildIndexInfo(indexRel);

	state->tupDesc = tupDesc;	/* assume we need not copy tupDesc */

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	if (state->indexInfo->ii_Expressions != NULL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * We will need to use FormIndexDatum to evaluate the index
		 * expressions.  To do that, we need an EState, as well as a
		 * TupleTableSlot to put the table tuples into.  The econtext's
		 * scantuple has to point to that slot, too.
		 */
		state->estate = CreateExecutorState();
		slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);
		econtext = GetPerTupleExprContext(state->estate);
		econtext->ecxt_scantuple = slot;
	}

	/* Prepare SortSupport data for each column */
	state->sortKeys = (SortSupport) palloc0(state->nKeys *
											sizeof(SortSupportData));

	for (i = 0; i < state->nKeys; i++)
	{
		SortSupport sortKey = state->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0);

		AssertState(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_btree(Relation heapRel,
							Relation indexRel,
							bool enforceUnique,
							int workMem,
							SortCoordinate coordinate,
							bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   randomAccess);
	BTScanInsert indexScanKey;
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(state->maincontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: unique = %c, workMem = %d, randomAccess = %c",
			 enforceUnique ? 't' : 'f',
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(INDEX_SORT,
								enforceUnique,
								state->nKeys,
								workMem,
								randomAccess,
								PARALLEL_SORT(state));

	state->comparetup = comparetup_index_btree;
	state->copytup = copytup_index;
	state->writetup = writetup_index;
	state->readtup = readtup_index;
	state->abbrevNext = 10;

	state->heapRel = heapRel;
	state->indexRel = indexRel;
	state->enforceUnique = enforceUnique;

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	/* Prepare SortSupport data for each column */
	state->sortKeys = (SortSupport) palloc0(state->nKeys *
											sizeof(SortSupportData));

	for (i = 0; i < state->nKeys; i++)
	{
		SortSupport sortKey = state->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0);

		AssertState(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_hash(Relation heapRel,
						   Relation indexRel,
						   uint32 high_mask,
						   uint32 low_mask,
						   uint32 max_buckets,
						   int workMem,
						   SortCoordinate coordinate,
						   bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   randomAccess);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(state->maincontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: high_mask = 0x%x, low_mask = 0x%x, "
			 "max_buckets = 0x%x, workMem = %d, randomAccess = %c",
			 high_mask,
			 low_mask,
			 max_buckets,
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = 1;			/* Only one sort column, the hash code */

	state->comparetup = comparetup_index_hash;
	state->copytup = copytup_index;
	state->writetup = writetup_index;
	state->readtup = readtup_index;

	state->heapRel = heapRel;
	state->indexRel = indexRel;

	state->high_mask = high_mask;
	state->low_mask = low_mask;
	state->max_buckets = max_buckets;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_datum(Oid datumType, Oid sortOperator, Oid sortCollation,
					  bool nullsFirstFlag, int workMem,
					  SortCoordinate coordinate, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   randomAccess);
	MemoryContext oldcontext;
	int16		typlen;
	bool		typbyval;

	oldcontext = MemoryContextSwitchTo(state->maincontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin datum sort: workMem = %d, randomAccess = %c",
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = 1;			/* always a one-column sort */

	TRACE_POSTGRESQL_SORT_START(DATUM_SORT,
								false,	/* no unique check */
								1,
								workMem,
								randomAccess,
								PARALLEL_SORT(state));

	state->comparetup = comparetup_datum;
	state->copytup = copytup_datum;
	state->writetup = writetup_datum;
	state->readtup = readtup_datum;
	state->abbrevNext = 10;

	state->datumType = datumType;

	/* lookup necessary attributes of the datum type */
	get_typlenbyval(datumType, &typlen, &typbyval);
	state->datumTypeLen = typlen;
	state->tuples = !typbyval;

	/* Prepare SortSupport data */
	state->sortKeys = (SortSupport) palloc0(sizeof(SortSupportData));

	state->sortKeys->ssup_cxt = CurrentMemoryContext;
	state->sortKeys->ssup_collation = sortCollation;
	state->sortKeys->ssup_nulls_first = nullsFirstFlag;

	/*
	 * Abbreviation is possible here only for by-reference types.  In theory,
	 * a pass-by-value datatype could have an abbreviated form that is cheaper
	 * to compare.  In a tuple sort, we could support that, because we can
	 * always extract the original datum from the tuple as needed.  Here, we
	 * can't, because a datum sort only stores a single copy of the datum; the
	 * "tuple" field of each SortTuple is NULL.
	 */
	state->sortKeys->abbreviate = !typbyval;

	PrepareSortSupportFromOrderingOp(sortOperator, state->sortKeys);

	/*
	 * The "onlyKey" optimization cannot be used with abbreviated keys, since
	 * tie-breaker comparisons may be required.  Typically, the optimization
	 * is only of value to pass-by-value types anyway, whereas abbreviated
	 * keys are typically only of value to pass-by-reference types.
	 */
	if (!state->sortKeys->abbrev_converter)
		state->onlyKey = state->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 * tuplesort_set_bound
 *
 *	Advise tuplesort that at most the first N result tuples are required.
 *
 * Must be called before inserting any tuples.  (Actually, we could allow it
 * as long as the sort hasn't spilled to disk, but there seems no need for
 * delayed calls at the moment.)
 *
 * This is a hint only. The tuplesort may still return more tuples than
 * requested.  Parallel leader tuplesorts will always ignore the hint.
 */
void
tuplesort_set_bound(Tuplesortstate *state, int64 bound)
{
	/* Assert we're called before loading any tuples */
	Assert(state->status == TSS_INITIAL && state->memtupcount == 0);
	/* Can't set the bound twice, either */
	Assert(!state->bounded);
	/* Also, this shouldn't be called in a parallel worker */
	Assert(!WORKER(state));

	/* Parallel leader allows but ignores hint */
	if (LEADER(state))
		return;

#ifdef DEBUG_BOUNDED_SORT
	/* Honor GUC setting that disables the feature (for easy testing) */
	if (!optimize_bounded_sort)
		return;
#endif

	/* We want to be able to compute bound * 2, so limit the setting */
	if (bound > (int64) (INT_MAX / 2))
		return;

	state->bounded = true;
	state->bound = (int) bound;

	/*
	 * Bounded sorts are not an effective target for abbreviated key
	 * optimization.  Disable by setting state to be consistent with no
	 * abbreviation support.
	 */
	state->sortKeys->abbrev_converter = NULL;
	if (state->sortKeys->abbrev_full_comparator)
		state->sortKeys->comparator = state->sortKeys->abbrev_full_comparator;

	/* Not strictly necessary, but be tidy */
	state->sortKeys->abbrev_abort = NULL;
	state->sortKeys->abbrev_full_comparator = NULL;
}

/*
 * tuplesort_used_bound
 *
 * Allow callers to find out if the sort state was able to use a bound.
 */
bool
tuplesort_used_bound(Tuplesortstate *state)
{
	return state->boundUsed;
}

/*
 * tuplesort_free
 *
 *	Internal routine for freeing resources of tuplesort.
 */
static void
tuplesort_free(Tuplesortstate *state)
{
	/* context swap probably not needed, but let's be safe */
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	long		spaceUsed;

	if (state->tapeset)
		spaceUsed = LogicalTapeSetBlocks(state->tapeset);
	else
		spaceUsed = (state->allowedMem - state->availMem + 1023) / 1024;
#endif

	/*
	 * Delete temporary "tape" files, if any.
	 *
	 * Note: want to include this in reported total cost of sort, hence need
	 * for two #ifdef TRACE_SORT sections.
	 */
	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->tapeset)
			elog(LOG, "%s of worker %d ended, %ld disk blocks used: %s",
				 SERIAL(state) ? "external sort" : "parallel external sort",
				 state->worker, spaceUsed, pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "%s of worker %d ended, %ld KB used: %s",
				 SERIAL(state) ? "internal sort" : "unperformed parallel sort",
				 state->worker, spaceUsed, pg_rusage_show(&state->ru_start));
	}

	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, spaceUsed);
#else

	/*
	 * If you disabled TRACE_SORT, you can still probe sort__done, but you
	 * ain't getting space-used stats.
	 */
	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, 0L);
#endif

	/* Free any execution state created for CLUSTER case */
	if (state->estate != NULL)
	{
		ExprContext *econtext = GetPerTupleExprContext(state->estate);

		ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
		FreeExecutorState(state->estate);
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Free the per-sort memory context, thereby releasing all working memory.
	 */
	MemoryContextReset(state->sortcontext);
}

/*
 * tuplesort_end
 *
 *	Release resources and clean up.
 *
 * NOTE: after calling this, any pointers returned by tuplesort_getXXX are
 * pointing to garbage.  Be careful not to attempt to use or free such
 * pointers afterwards!
 */
void
tuplesort_end(Tuplesortstate *state)
{
	tuplesort_free(state);

	/*
	 * Free the main memory context, including the Tuplesortstate struct
	 * itself.
	 */
	MemoryContextDelete(state->maincontext);
}

/*
 * tuplesort_updatemax
 *
 *	Update maximum resource usage statistics.
 */
static void
tuplesort_updatemax(Tuplesortstate *state)
{
	int64		spaceUsed;
	bool		isSpaceDisk;

	/*
	 * Note: it might seem we should provide both memory and disk usage for a
	 * disk-based sort.  However, the current code doesn't track memory space
	 * accurately once we have begun to return tuples to the caller (since we
	 * don't account for pfree's the caller is expected to do), so we cannot
	 * rely on availMem in a disk sort.  This does not seem worth the overhead
	 * to fix.  Is it worth creating an API for the memory context code to
	 * tell us how much is actually used in sortcontext?
	 */
	if (state->tapeset)
	{
		isSpaceDisk = true;
		spaceUsed = LogicalTapeSetBlocks(state->tapeset) * BLCKSZ;
	}
	else
	{
		isSpaceDisk = false;
		spaceUsed = state->allowedMem - state->availMem;
	}

	/*
	 * Sort evicts data to the disk when it wasn't able to fit that data into
	 * main memory.  This is why we assume space used on the disk to be more
	 * important for tracking resource usage than space used in memory. Note
	 * that the amount of space occupied by some tupleset on the disk might be
	 * less than amount of space occupied by the same tupleset in memory due
	 * to more compact representation.
	 */
	if ((isSpaceDisk && !state->isMaxSpaceDisk) ||
		(isSpaceDisk == state->isMaxSpaceDisk && spaceUsed > state->maxSpace))
	{
		state->maxSpace = spaceUsed;
		state->isMaxSpaceDisk = isSpaceDisk;
		state->maxSpaceStatus = state->status;
	}
}

/*
 * tuplesort_reset
 *
 *	Reset the tuplesort.  Reset all the data in the tuplesort, but leave the
 *	meta-information in.  After tuplesort_reset, tuplesort is ready to start
 *	a new sort.  This allows avoiding recreation of tuple sort states (and
 *	save resources) when sorting multiple small batches.
 */
void
tuplesort_reset(Tuplesortstate *state)
{
	tuplesort_updatemax(state);
	tuplesort_free(state);

	/*
	 * After we've freed up per-batch memory, re-setup all of the state common
	 * to both the first batch and any subsequent batch.
	 */
	tuplesort_begin_batch(state);

	state->lastReturnedTuple = NULL;
	state->slabMemoryBegin = NULL;
	state->slabMemoryEnd = NULL;
	state->slabFreeHead = NULL;
}

/*
 * Grow the memtuples[] array, if possible within our memory constraint.  We
 * must not exceed INT_MAX tuples in memory or the caller-provided memory
 * limit.  Return true if we were able to enlarge the array, false if not.
 *
 * Normally, at each increment we double the size of the array.  When doing
 * that would exceed a limit, we attempt one last, smaller increase (and then
 * clear the growmemtuples flag so we don't try any more).  That allows us to
 * use memory as fully as permitted; sticking to the pure doubling rule could
 * result in almost half going unused.  Because availMem moves around with
 * tuple addition/removal, we need some rule to prevent making repeated small
 * increases in memtupsize, which would just be useless thrashing.  The
 * growmemtuples flag accomplishes that and also prevents useless
 * recalculations in this function.
 */
static bool
grow_memtuples(Tuplesortstate *state)
{
	int			newmemtupsize;
	int			memtupsize = state->memtupsize;
	int64		memNowUsed = state->allowedMem - state->availMem;

	/* Forget it if we've already maxed out memtuples, per comment above */
	if (!state->growmemtuples)
		return false;

	/* Select new value of memtupsize */
	if (memNowUsed <= state->availMem)
	{
		/*
		 * We've used no more than half of allowedMem; double our usage,
		 * clamping at INT_MAX tuples.
		 */
		if (memtupsize < INT_MAX / 2)
			newmemtupsize = memtupsize * 2;
		else
		{
			newmemtupsize = INT_MAX;
			state->growmemtuples = false;
		}
	}
	else
	{
		/*
		 * This will be the last increment of memtupsize.  Abandon doubling
		 * strategy and instead increase as much as we safely can.
		 *
		 * To stay within allowedMem, we can't increase memtupsize by more
		 * than availMem / sizeof(SortTuple) elements.  In practice, we want
		 * to increase it by considerably less, because we need to leave some
		 * space for the tuples to which the new array slots will refer.  We
		 * assume the new tuples will be about the same size as the tuples
		 * we've already seen, and thus we can extrapolate from the space
		 * consumption so far to estimate an appropriate new size for the
		 * memtuples array.  The optimal value might be higher or lower than
		 * this estimate, but it's hard to know that in advance.  We again
		 * clamp at INT_MAX tuples.
		 *
		 * This calculation is safe against enlarging the array so much that
		 * LACKMEM becomes true, because the memory currently used includes
		 * the present array; thus, there would be enough allowedMem for the
		 * new array elements even if no other memory were currently used.
		 *
		 * We do the arithmetic in float8, because otherwise the product of
		 * memtupsize and allowedMem could overflow.  Any inaccuracy in the
		 * result should be insignificant; but even if we computed a
		 * completely insane result, the checks below will prevent anything
		 * really bad from happening.
		 */
		double		grow_ratio;

		grow_ratio = (double) state->allowedMem / (double) memNowUsed;
		if (memtupsize * grow_ratio < INT_MAX)
			newmemtupsize = (int) (memtupsize * grow_ratio);
		else
			newmemtupsize = INT_MAX;

		/* We won't make any further enlargement attempts */
		state->growmemtuples = false;
	}

	/* Must enlarge array by at least one element, else report failure */
	if (newmemtupsize <= memtupsize)
		goto noalloc;

	/*
	 * On a 32-bit machine, allowedMem could exceed MaxAllocHugeSize.  Clamp
	 * to ensure our request won't be rejected.  Note that we can easily
	 * exhaust address space before facing this outcome.  (This is presently
	 * impossible due to guc.c's MAX_KILOBYTES limitation on work_mem, but
	 * don't rely on that at this distance.)
	 */
	if ((Size) newmemtupsize >= MaxAllocHugeSize / sizeof(SortTuple))
	{
		newmemtupsize = (int) (MaxAllocHugeSize / sizeof(SortTuple));
		state->growmemtuples = false;	/* can't grow any more */
	}

	/*
	 * We need to be sure that we do not cause LACKMEM to become true, else
	 * the space management algorithm will go nuts.  The code above should
	 * never generate a dangerous request, but to be safe, check explicitly
	 * that the array growth fits within availMem.  (We could still cause
	 * LACKMEM if the memory chunk overhead associated with the memtuples
	 * array were to increase.  That shouldn't happen because we chose the
	 * initial array size large enough to ensure that palloc will be treating
	 * both old and new arrays as separate chunks.  But we'll check LACKMEM
	 * explicitly below just in case.)
	 */
	if (state->availMem < (int64) ((newmemtupsize - memtupsize) * sizeof(SortTuple)))
		goto noalloc;

	/* OK, do it */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	state->memtupsize = newmemtupsize;
	state->memtuples = (SortTuple *)
		repalloc_huge(state->memtuples,
					  state->memtupsize * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	if (LACKMEM(state))
		elog(ERROR, "unexpected out-of-memory situation in tuplesort");
	return true;

noalloc:
	/* If for any reason we didn't realloc, shut off future attempts */
	state->growmemtuples = false;
	return false;
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 * Then call the common code.
	 */
	COPYTUP(state, &stup, (void *) slot);

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_putheaptuple(Tuplesortstate *state, HeapTuple tup)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 * Then call the common code.
	 */
	COPYTUP(state, &stup, (void *) tup);

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Collect one index tuple while collecting input data for sort, building
 * it from caller-supplied values.
 */
void
tuplesort_putindextuplevalues(Tuplesortstate *state, Relation rel,
							  ItemPointer self, Datum *values,
							  bool *isnull)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->tuplecontext);
	SortTuple	stup;
	Datum		original;
	IndexTuple	tuple;

	stup.tuple = index_form_tuple(RelationGetDescr(rel), values, isnull);
	tuple = ((IndexTuple) stup.tuple);
	tuple->t_tid = *self;
	USEMEM(state, GetMemoryChunkSpace(stup.tuple));
	/* set up first-column key value */
	original = index_getattr(tuple,
							 1,
							 RelationGetDescr(state->indexRel),
							 &stup.isnull1);

	MemoryContextSwitchTo(state->sortcontext);

	if (!state->sortKeys || !state->sortKeys->abbrev_converter || stup.isnull1)
	{
		/*
		 * Store ordinary Datum representation, or NULL value.  If there is a
		 * converter it won't expect NULL values, and cost model is not
		 * required to account for NULL, so in that case we avoid calling
		 * converter and just set datum1 to zeroed representation (to be
		 * consistent, and to support cheap inequality tests for NULL
		 * abbreviated keys).
		 */
		stup.datum1 = original;
	}
	else if (!consider_abort_common(state))
	{
		/* Store abbreviated key representation */
		stup.datum1 = state->sortKeys->abbrev_converter(original,
														state->sortKeys);
	}
	else
	{
		/* Abort abbreviation */
		int			i;

		stup.datum1 = original;

		/*
		 * Set state to be consistent with never trying abbreviation.
		 *
		 * Alter datum1 representation in already-copied tuples, so as to
		 * ensure a consistent representation (current tuple was just
		 * handled).  It does not matter if some dumped tuples are already
		 * sorted on tape, since serialized tuples lack abbreviated keys
		 * (TSS_BUILDRUNS state prevents control reaching here in any case).
		 */
		for (i = 0; i < state->memtupcount; i++)
		{
			SortTuple  *mtup = &state->memtuples[i];

			tuple = mtup->tuple;
			mtup->datum1 = index_getattr(tuple,
										 1,
										 RelationGetDescr(state->indexRel),
										 &mtup->isnull1);
		}
	}

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
void
tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->tuplecontext);
	SortTuple	stup;

	/*
	 * Pass-by-value types or null values are just stored directly in
	 * stup.datum1 (and stup.tuple is not used and set to NULL).
	 *
	 * Non-null pass-by-reference values need to be copied into memory we
	 * control, and possibly abbreviated. The copied value is pointed to by
	 * stup.tuple and is treated as the canonical copy (e.g. to return via
	 * tuplesort_getdatum or when writing to tape); stup.datum1 gets the
	 * abbreviated value if abbreviation is happening, otherwise it's
	 * identical to stup.tuple.
	 */

	if (isNull || !state->tuples)
	{
		/*
		 * Set datum1 to zeroed representation for NULLs (to be consistent,
		 * and to support cheap inequality tests for NULL abbreviated keys).
		 */
		stup.datum1 = !isNull ? val : (Datum) 0;
		stup.isnull1 = isNull;
		stup.tuple = NULL;		/* no separate storage */
		MemoryContextSwitchTo(state->sortcontext);
	}
	else
	{
		Datum		original = datumCopy(val, false, state->datumTypeLen);

		stup.isnull1 = false;
		stup.tuple = DatumGetPointer(original);
		USEMEM(state, GetMemoryChunkSpace(stup.tuple));
		MemoryContextSwitchTo(state->sortcontext);

		if (!state->sortKeys->abbrev_converter)
		{
			stup.datum1 = original;
		}
		else if (!consider_abort_common(state))
		{
			/* Store abbreviated key representation */
			stup.datum1 = state->sortKeys->abbrev_converter(original,
															state->sortKeys);
		}
		else
		{
			/* Abort abbreviation */
			int			i;

			stup.datum1 = original;

			/*
			 * Set state to be consistent with never trying abbreviation.
			 *
			 * Alter datum1 representation in already-copied tuples, so as to
			 * ensure a consistent representation (current tuple was just
			 * handled).  It does not matter if some dumped tuples are already
			 * sorted on tape, since serialized tuples lack abbreviated keys
			 * (TSS_BUILDRUNS state prevents control reaching here in any
			 * case).
			 */
			for (i = 0; i < state->memtupcount; i++)
			{
				SortTuple  *mtup = &state->memtuples[i];

				mtup->datum1 = PointerGetDatum(mtup->tuple);
			}
		}
	}

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Shared code for tuple and datum cases.
 */
static void
puttuple_common(Tuplesortstate *state, SortTuple *tuple)
{
	Assert(!LEADER(state));

	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * Save the tuple into the unsorted array.  First, grow the array
			 * as needed.  Note that we try to grow the array when there is
			 * still one free slot remaining --- if we fail, there'll still be
			 * room to store the incoming tuple, and then we'll switch to
			 * tape-based operation.
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				(void) grow_memtuples(state);
				Assert(state->memtupcount < state->memtupsize);
			}
			state->memtuples[state->memtupcount++] = *tuple;

			/*
			 * Check if it's time to switch over to a bounded heapsort. We do
			 * so if the input tuple count exceeds twice the desired tuple
			 * count (this is a heuristic for where heapsort becomes cheaper
			 * than a quicksort), or if we've just filled workMem and have
			 * enough tuples to meet the bound.
			 *
			 * Note that once we enter TSS_BOUNDED state we will always try to
			 * complete the sort that way.  In the worst case, if later input
			 * tuples are larger than earlier ones, this might cause us to
			 * exceed workMem significantly.
			 */
			if (state->bounded &&
				(state->memtupcount > state->bound * 2 ||
				 (state->memtupcount > state->bound && LACKMEM(state))))
			{
#ifdef TRACE_SORT
				if (trace_sort)
					elog(LOG, "switching to bounded heapsort at %d tuples: %s",
						 state->memtupcount,
						 pg_rusage_show(&state->ru_start));
#endif
				make_bounded_heap(state);
				return;
			}

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.
			 */
			inittapes(state, true);

			/*
			 * Dump all tuples.
			 */
			dumptuples(state, false);
			break;

		case TSS_BOUNDED:

			/*
			 * We don't want to grow the array here, so check whether the new
			 * tuple can be discarded before putting it in.  This should be a
			 * good speed optimization, too, since when there are many more
			 * input tuples than the bound, most input tuples can be discarded
			 * with just this one comparison.  Note that because we currently
			 * have the sort direction reversed, we must check for <= not >=.
			 */
			if (COMPARETUP(state, tuple, &state->memtuples[0]) <= 0)
			{
				/* new tuple <= top of the heap, so we can discard it */
				free_sort_tuple(state, tuple);
				CHECK_FOR_INTERRUPTS();
			}
			else
			{
				/* discard top of heap, replacing it with the new tuple */
				free_sort_tuple(state, &state->memtuples[0]);
				tuplesort_heap_replace_top(state, tuple);
			}
			break;

		case TSS_BUILDRUNS:

			/*
			 * Save the tuple into the unsorted array (there must be space)
			 */
			state->memtuples[state->memtupcount++] = *tuple;

			/*
			 * If we are over the memory limit, dump all tuples.
			 */
			dumptuples(state, false);
			break;

		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
}

static bool
consider_abort_common(Tuplesortstate *state)
{
	Assert(state->sortKeys[0].abbrev_converter != NULL);
	Assert(state->sortKeys[0].abbrev_abort != NULL);
	Assert(state->sortKeys[0].abbrev_full_comparator != NULL);

	/*
	 * Check effectiveness of abbreviation optimization.  Consider aborting
	 * when still within memory limit.
	 */
	if (state->status == TSS_INITIAL &&
		state->memtupcount >= state->abbrevNext)
	{
		state->abbrevNext *= 2;

		/*
		 * Check opclass-supplied abbreviation abort routine.  It may indicate
		 * that abbreviation should not proceed.
		 */
		if (!state->sortKeys->abbrev_abort(state->memtupcount,
										   state->sortKeys))
			return false;

		/*
		 * Finally, restore authoritative comparator, and indicate that
		 * abbreviation is not in play by setting abbrev_converter to NULL
		 */
		state->sortKeys[0].comparator = state->sortKeys[0].abbrev_full_comparator;
		state->sortKeys[0].abbrev_converter = NULL;
		/* Not strictly necessary, but be tidy */
		state->sortKeys[0].abbrev_abort = NULL;
		state->sortKeys[0].abbrev_full_comparator = NULL;

		/* Give up - expect original pass-by-value representation */
		return true;
	}

	return false;
}

/*
 * All tuples have been provided; finish the sort.
 */
void
tuplesort_performsort(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "performsort of worker %d starting: %s",
			 state->worker, pg_rusage_show(&state->ru_start));
#endif

	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * We were able to accumulate all the tuples within the allowed
			 * amount of memory, or leader to take over worker tapes
			 */
			if (SERIAL(state))
			{
				/* Just qsort 'em and we're done */
				tuplesort_sort_memtuples(state);
				state->status = TSS_SORTEDINMEM;
			}
			else if (WORKER(state))
			{
				/*
				 * Parallel workers must still dump out tuples to tape.  No
				 * merge is required to produce single output run, though.
				 */
				inittapes(state, false);
				dumptuples(state, true);
				worker_nomergeruns(state);
				state->status = TSS_SORTEDONTAPE;
			}
			else
			{
				/*
				 * Leader will take over worker tapes and merge worker runs.
				 * Note that mergeruns sets the correct state->status.
				 */
				leader_takeover_tapes(state);
				mergeruns(state);
			}
			state->current = 0;
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;

		case TSS_BOUNDED:

			/*
			 * We were able to accumulate all the tuples required for output
			 * in memory, using a heap to eliminate excess tuples.  Now we
			 * have to transform the heap to a properly-sorted array.
			 */
			sort_bounded_heap(state);
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			state->status = TSS_SORTEDINMEM;
			break;

		case TSS_BUILDRUNS:

			/*
			 * Finish tape-based sort.  First, flush all tuples remaining in
			 * memory out to tape; then merge until we have a single remaining
			 * run (or, if !randomAccess and !WORKER(), one run per tape).
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

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->status == TSS_FINALMERGE)
			elog(LOG, "performsort of worker %d done (except %d-way final merge): %s",
				 state->worker, state->activeTapes,
				 pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "performsort of worker %d done: %s",
				 state->worker, pg_rusage_show(&state->ru_start));
	}
#endif

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Internal routine to fetch the next tuple in either forward or back
 * direction into *stup.  Returns false if no more tuples.
 * Returned tuple belongs to tuplesort memory context, and must not be freed
 * by caller.  Note that fetched tuple is stored in memory that may be
 * recycled by any future fetch.
 */
static bool
tuplesort_gettuple_common(Tuplesortstate *state, bool forward,
						  SortTuple *stup)
{
	unsigned int tuplen;
	size_t		nmoved;

	Assert(!WORKER(state));

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			Assert(forward || state->randomAccess);
			Assert(!state->slabAllocatorUsed);
			if (forward)
			{
				if (state->current < state->memtupcount)
				{
					*stup = state->memtuples[state->current++];
					return true;
				}
				state->eof_reached = true;

				/*
				 * Complain if caller tries to retrieve more tuples than
				 * originally asked for in a bounded sort.  This is because
				 * returning EOF here might be the wrong thing.
				 */
				if (state->bounded && state->current >= state->bound)
					elog(ERROR, "retrieved too many tuples in a bounded sort");

				return false;
			}
			else
			{
				if (state->current <= 0)
					return false;

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
						return false;
				}
				*stup = state->memtuples[state->current - 1];
				return true;
			}
			break;

		case TSS_SORTEDONTAPE:
			Assert(forward || state->randomAccess);
			Assert(state->slabAllocatorUsed);

			/*
			 * The slot that held the tuple that we returned in previous
			 * gettuple call can now be reused.
			 */
			if (state->lastReturnedTuple)
			{
				RELEASE_SLAB_SLOT(state, state->lastReturnedTuple);
				state->lastReturnedTuple = NULL;
			}

			if (forward)
			{
				if (state->eof_reached)
					return false;

				if ((tuplen = getlen(state, state->result_tape, true)) != 0)
				{
					READTUP(state, stup, state->result_tape, tuplen);

					/*
					 * Remember the tuple we return, so that we can recycle
					 * its memory on next call.  (This can be NULL, in the
					 * !state->tuples case).
					 */
					state->lastReturnedTuple = stup->tuple;

					return true;
				}
				else
				{
					state->eof_reached = true;
					return false;
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
				 * Seek position is pointing just past the zero tuplen at the
				 * end of file; back up to fetch last tuple's ending length
				 * word.  If seek fails we must have a completely empty file.
				 */
				nmoved = LogicalTapeBackspace(state->tapeset,
											  state->result_tape,
											  2 * sizeof(unsigned int));
				if (nmoved == 0)
					return false;
				else if (nmoved != 2 * sizeof(unsigned int))
					elog(ERROR, "unexpected tape position");
				state->eof_reached = false;
			}
			else
			{
				/*
				 * Back up and fetch previously-returned tuple's ending length
				 * word.  If seek fails, assume we are at start of file.
				 */
				nmoved = LogicalTapeBackspace(state->tapeset,
											  state->result_tape,
											  sizeof(unsigned int));
				if (nmoved == 0)
					return false;
				else if (nmoved != sizeof(unsigned int))
					elog(ERROR, "unexpected tape position");
				tuplen = getlen(state, state->result_tape, false);

				/*
				 * Back up to get ending length word of tuple before it.
				 */
				nmoved = LogicalTapeBackspace(state->tapeset,
											  state->result_tape,
											  tuplen + 2 * sizeof(unsigned int));
				if (nmoved == tuplen + sizeof(unsigned int))
				{
					/*
					 * We backed up over the previous tuple, but there was no
					 * ending length word before it.  That means that the prev
					 * tuple is the first tuple in the file.  It is now the
					 * next to read in forward direction (not obviously right,
					 * but that is what in-memory case does).
					 */
					return false;
				}
				else if (nmoved != tuplen + 2 * sizeof(unsigned int))
					elog(ERROR, "bogus tuple length in backward scan");
			}

			tuplen = getlen(state, state->result_tape, false);

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			nmoved = LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  tuplen);
			if (nmoved != tuplen)
				elog(ERROR, "bogus tuple length in backward scan");
			READTUP(state, stup, state->result_tape, tuplen);

			/*
			 * Remember the tuple we return, so that we can recycle its memory
			 * on next call. (This can be NULL, in the Datum case).
			 */
			state->lastReturnedTuple = stup->tuple;

			return true;

		case TSS_FINALMERGE:
			Assert(forward);
			/* We are managing memory ourselves, with the slab allocator. */
			Assert(state->slabAllocatorUsed);

			/*
			 * The slab slot holding the tuple that we returned in previous
			 * gettuple call can now be reused.
			 */
			if (state->lastReturnedTuple)
			{
				RELEASE_SLAB_SLOT(state, state->lastReturnedTuple);
				state->lastReturnedTuple = NULL;
			}

			/*
			 * This code should match the inner loop of mergeonerun().
			 */
			if (state->memtupcount > 0)
			{
				int			srcTape = state->memtuples[0].srctape;
				SortTuple	newtup;

				*stup = state->memtuples[0];

				/*
				 * Remember the tuple we return, so that we can recycle its
				 * memory on next call. (This can be NULL, in the Datum case).
				 */
				state->lastReturnedTuple = stup->tuple;

				/*
				 * Pull next tuple from tape, and replace the returned tuple
				 * at top of the heap with it.
				 */
				if (!mergereadnext(state, srcTape, &newtup))
				{
					/*
					 * If no more data, we've reached end of run on this tape.
					 * Remove the top node from the heap.
					 */
					tuplesort_heap_delete_top(state);

					/*
					 * Rewind to free the read buffer.  It'd go away at the
					 * end of the sort anyway, but better to release the
					 * memory early.
					 */
					LogicalTapeRewindForWrite(state->tapeset, srcTape);
					return true;
				}
				newtup.srctape = srcTape;
				tuplesort_heap_replace_top(state, &newtup);
				return true;
			}
			return false;

		default:
			elog(ERROR, "invalid tuplesort state");
			return false;		/* keep compiler quiet */
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * If successful, put tuple in slot and return true; else, clear the slot
 * and return false.
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value in leading attribute will set abbreviated value to zeroed
 * representation, which caller may rely on in abbreviated inequality check.
 *
 * If copy is true, the slot receives a tuple that's been copied into the
 * caller's memory context, so that it will stay valid regardless of future
 * manipulations of the tuplesort's state (up to and including deleting the
 * tuplesort).  If copy is false, the slot will just receive a pointer to a
 * tuple held within the tuplesort, which is more efficient, but only safe for
 * callers that are prepared to have any subsequent manipulation of the
 * tuplesort's state invalidate slot contents.
 */
bool
tuplesort_gettupleslot(Tuplesortstate *state, bool forward, bool copy,
					   TupleTableSlot *slot, Datum *abbrev)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	if (stup.tuple)
	{
		/* Record abbreviated key for caller */
		if (state->sortKeys->abbrev_converter && abbrev)
			*abbrev = stup.datum1;

		if (copy)
			stup.tuple = heap_copy_minimal_tuple((MinimalTuple) stup.tuple);

		ExecStoreMinimalTuple((MinimalTuple) stup.tuple, slot, copy);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.  Returned tuple belongs to tuplesort memory
 * context, and must not be freed by caller.  Caller may not rely on tuple
 * remaining valid after any further manipulation of tuplesort.
 */
HeapTuple
tuplesort_getheaptuple(Tuplesortstate *state, bool forward)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return stup.tuple;
}

/*
 * Fetch the next index tuple in either forward or back direction.
 * Returns NULL if no more tuples.  Returned tuple belongs to tuplesort memory
 * context, and must not be freed by caller.  Caller may not rely on tuple
 * remaining valid after any further manipulation of tuplesort.
 */
IndexTuple
tuplesort_getindextuple(Tuplesortstate *state, bool forward)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return (IndexTuple) stup.tuple;
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns false if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * in caller's context, and is now owned by the caller (this differs from
 * similar routines for other types of tuplesorts).
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value will have a zeroed abbreviated value representation, which caller
 * may rely on in abbreviated inequality check.
 */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull, Datum *abbrev)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
	{
		MemoryContextSwitchTo(oldcontext);
		return false;
	}

	/* Ensure we copy into caller's memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Record abbreviated key for caller */
	if (state->sortKeys->abbrev_converter && abbrev)
		*abbrev = stup.datum1;

	if (stup.isnull1 || !state->tuples)
	{
		*val = stup.datum1;
		*isNull = stup.isnull1;
	}
	else
	{
		/* use stup.tuple because stup.datum1 may be an abbreviation */
		*val = datumCopy(PointerGetDatum(stup.tuple), false, state->datumTypeLen);
		*isNull = false;
	}

	return true;
}

/*
 * Advance over N tuples in either forward or back direction,
 * without returning any data.  N==0 is a no-op.
 * Returns true if successful, false if ran out of tuples.
 */
bool
tuplesort_skiptuples(Tuplesortstate *state, int64 ntuples, bool forward)
{
	MemoryContext oldcontext;

	/*
	 * We don't actually support backwards skip yet, because no callers need
	 * it.  The API is designed to allow for that later, though.
	 */
	Assert(forward);
	Assert(ntuples >= 0);
	Assert(!WORKER(state));

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			if (state->memtupcount - state->current >= ntuples)
			{
				state->current += ntuples;
				return true;
			}
			state->current = state->memtupcount;
			state->eof_reached = true;

			/*
			 * Complain if caller tries to retrieve more tuples than
			 * originally asked for in a bounded sort.  This is because
			 * returning EOF here might be the wrong thing.
			 */
			if (state->bounded && state->current >= state->bound)
				elog(ERROR, "retrieved too many tuples in a bounded sort");

			return false;

		case TSS_SORTEDONTAPE:
		case TSS_FINALMERGE:

			/*
			 * We could probably optimize these cases better, but for now it's
			 * not worth the trouble.
			 */
			oldcontext = MemoryContextSwitchTo(state->sortcontext);
			while (ntuples-- > 0)
			{
				SortTuple	stup;

				if (!tuplesort_gettuple_common(state, forward, &stup))
				{
					MemoryContextSwitchTo(oldcontext);
					return false;
				}
				CHECK_FOR_INTERRUPTS();
			}
			MemoryContextSwitchTo(oldcontext);
			return true;

		default:
			elog(ERROR, "invalid tuplesort state");
			return false;		/* keep compiler quiet */
	}
}

/*
 * tuplesort_merge_order - report merge order we'll use for given memory
 * (note: "merge order" just means the number of input tapes in the merge).
 *
 * This is exported for use by the planner.  allowedMem is in bytes.
 */
int
tuplesort_merge_order(int64 allowedMem)
{
	int			mOrder;

	/*
	 * We need one tape for each merge input, plus another one for the output,
	 * and each of these tapes needs buffer space.  In addition we want
	 * MERGE_BUFFER_SIZE workspace per input tape (but the output tape doesn't
	 * count).
	 *
	 * Note: you might be thinking we need to account for the memtuples[]
	 * array in this calculation, but we effectively treat that as part of the
	 * MERGE_BUFFER_SIZE workspace.
	 */
	mOrder = (allowedMem - TAPE_BUFFER_OVERHEAD) /
		(MERGE_BUFFER_SIZE + TAPE_BUFFER_OVERHEAD);

	/*
	 * Even in minimum memory, use at least a MINORDER merge.  On the other
	 * hand, even when we have lots of memory, do not use more than a MAXORDER
	 * merge.  Tapes are pretty cheap, but they're not entirely free.  Each
	 * additional tape reduces the amount of memory available to build runs,
	 * which in turn can cause the same sort to need more runs, which makes
	 * merging slower even if it can still be done in a single pass.  Also,
	 * high order merges are quite slow due to CPU cache effects; it can be
	 * faster to pay the I/O cost of a polyphase merge than to perform a
	 * single merge pass across many hundreds of tapes.
	 */
	mOrder = Max(mOrder, MINORDER);
	mOrder = Min(mOrder, MAXORDER);

	return mOrder;
}

/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we won't sort in memory.
 */
static void
inittapes(Tuplesortstate *state, bool mergeruns)
{
	int			maxTapes,
				j;

	Assert(!LEADER(state));

	if (mergeruns)
	{
		/* Compute number of tapes to use: merge order plus 1 */
		maxTapes = tuplesort_merge_order(state->allowedMem) + 1;
	}
	else
	{
		/* Workers can sometimes produce single run, output without merge */
		Assert(WORKER(state));
		maxTapes = MINORDER + 1;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d switching to external sort with %d tapes: %s",
			 state->worker, maxTapes, pg_rusage_show(&state->ru_start));
#endif

	/* Create the tape set and allocate the per-tape data arrays */
	inittapestate(state, maxTapes);
	state->tapeset =
		LogicalTapeSetCreate(maxTapes, false, NULL,
							 state->shared ? &state->shared->fileset : NULL,
							 state->worker);

	state->currentRun = 0;

	/*
	 * Initialize variables of Algorithm D (step D1).
	 */
	for (j = 0; j < maxTapes; j++)
	{
		state->tp_fib[j] = 1;
		state->tp_runs[j] = 0;
		state->tp_dummy[j] = 1;
		state->tp_tapenum[j] = j;
	}
	state->tp_fib[state->tapeRange] = 0;
	state->tp_dummy[state->tapeRange] = 0;

	state->Level = 1;
	state->destTape = 0;

	state->status = TSS_BUILDRUNS;
}

/*
 * inittapestate - initialize generic tape management state
 */
static void
inittapestate(Tuplesortstate *state, int maxTapes)
{
	int64		tapeSpace;

	/*
	 * Decrease availMem to reflect the space needed for tape buffers; but
	 * don't decrease it to the point that we have no room for tuples. (That
	 * case is only likely to occur if sorting pass-by-value Datums; in all
	 * other scenarios the memtuples[] array is unlikely to occupy more than
	 * half of allowedMem.  In the pass-by-value case it's not important to
	 * account for tuple space, so we don't care if LACKMEM becomes
	 * inaccurate.)
	 */
	tapeSpace = (int64) maxTapes * TAPE_BUFFER_OVERHEAD;

	if (tapeSpace + GetMemoryChunkSpace(state->memtuples) < state->allowedMem)
		USEMEM(state, tapeSpace);

	/*
	 * Make sure that the temp file(s) underlying the tape set are created in
	 * suitable temp tablespaces.  For parallel sorts, this should have been
	 * called already, but it doesn't matter if it is called a second time.
	 */
	PrepareTempTablespaces();

	state->mergeactive = (bool *) palloc0(maxTapes * sizeof(bool));
	state->tp_fib = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_runs = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_dummy = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_tapenum = (int *) palloc0(maxTapes * sizeof(int));

	/* Record # of tapes allocated (for duration of sort) */
	state->maxTapes = maxTapes;
	/* Record maximum # of tapes usable as inputs when merging */
	state->tapeRange = maxTapes - 1;
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
	for (j = 0; j < state->tapeRange; j++)
	{
		state->tp_dummy[j] = a + state->tp_fib[j + 1] - state->tp_fib[j];
		state->tp_fib[j] = a + state->tp_fib[j + 1];
	}
	state->destTape = 0;
}

/*
 * Initialize the slab allocation arena, for the given number of slots.
 */
static void
init_slab_allocator(Tuplesortstate *state, int numSlots)
{
	if (numSlots > 0)
	{
		char	   *p;
		int			i;

		state->slabMemoryBegin = palloc(numSlots * SLAB_SLOT_SIZE);
		state->slabMemoryEnd = state->slabMemoryBegin +
			numSlots * SLAB_SLOT_SIZE;
		state->slabFreeHead = (SlabSlot *) state->slabMemoryBegin;
		USEMEM(state, numSlots * SLAB_SLOT_SIZE);

		p = state->slabMemoryBegin;
		for (i = 0; i < numSlots - 1; i++)
		{
			((SlabSlot *) p)->nextfree = (SlabSlot *) (p + SLAB_SLOT_SIZE);
			p += SLAB_SLOT_SIZE;
		}
		((SlabSlot *) p)->nextfree = NULL;
	}
	else
	{
		state->slabMemoryBegin = state->slabMemoryEnd = NULL;
		state->slabFreeHead = NULL;
	}
	state->slabAllocatorUsed = true;
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
	int			numTapes;
	int			numInputTapes;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0);

	if (state->sortKeys != NULL && state->sortKeys->abbrev_converter != NULL)
	{
		/*
		 * If there are multiple runs to be merged, when we go to read back
		 * tuples from disk, abbreviated keys will not have been stored, and
		 * we don't care to regenerate them.  Disable abbreviation from this
		 * point on.
		 */
		state->sortKeys->abbrev_converter = NULL;
		state->sortKeys->comparator = state->sortKeys->abbrev_full_comparator;

		/* Not strictly necessary, but be tidy */
		state->sortKeys->abbrev_abort = NULL;
		state->sortKeys->abbrev_full_comparator = NULL;
	}

	/*
	 * Reset tuple memory.  We've freed all the tuples that we previously
	 * allocated.  We will use the slab allocator from now on.
	 */
	MemoryContextResetOnly(state->tuplecontext);

	/*
	 * We no longer need a large memtuples array.  (We will allocate a smaller
	 * one for the heap later.)
	 */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	pfree(state->memtuples);
	state->memtuples = NULL;

	/*
	 * If we had fewer runs than tapes, refund the memory that we imagined we
	 * would need for the tape buffers of the unused tapes.
	 *
	 * numTapes and numInputTapes reflect the actual number of tapes we will
	 * use.  Note that the output tape's tape number is maxTapes - 1, so the
	 * tape numbers of the used tapes are not consecutive, and you cannot just
	 * loop from 0 to numTapes to visit all used tapes!
	 */
	if (state->Level == 1)
	{
		numInputTapes = state->currentRun;
		numTapes = numInputTapes + 1;
		FREEMEM(state, (state->maxTapes - numTapes) * TAPE_BUFFER_OVERHEAD);
	}
	else
	{
		numInputTapes = state->tapeRange;
		numTapes = state->maxTapes;
	}

	/*
	 * Initialize the slab allocator.  We need one slab slot per input tape,
	 * for the tuples in the heap, plus one to hold the tuple last returned
	 * from tuplesort_gettuple.  (If we're sorting pass-by-val Datums,
	 * however, we don't need to do allocate anything.)
	 *
	 * From this point on, we no longer use the USEMEM()/LACKMEM() mechanism
	 * to track memory usage of individual tuples.
	 */
	if (state->tuples)
		init_slab_allocator(state, numInputTapes + 1);
	else
		init_slab_allocator(state, 0);

	/*
	 * Allocate a new 'memtuples' array, for the heap.  It will hold one tuple
	 * from each input tape.
	 */
	state->memtupsize = numInputTapes;
	state->memtuples = (SortTuple *) MemoryContextAlloc(state->maincontext,
														numInputTapes * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	/*
	 * Use all the remaining memory we have available for read buffers among
	 * the input tapes.
	 *
	 * We don't try to "rebalance" the memory among tapes, when we start a new
	 * merge phase, even if some tapes are inactive in the new phase.  That
	 * would be hard, because logtape.c doesn't know where one run ends and
	 * another begins.  When a new merge phase begins, and a tape doesn't
	 * participate in it, its buffer nevertheless already contains tuples from
	 * the next run on same tape, so we cannot release the buffer.  That's OK
	 * in practice, merge performance isn't that sensitive to the amount of
	 * buffers used, and most merge phases use all or almost all tapes,
	 * anyway.
	 */
#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d using " INT64_FORMAT " KB of memory for read buffers among %d input tapes",
			 state->worker, state->availMem / 1024, numInputTapes);
#endif

	state->read_buffer_size = Max(state->availMem / numInputTapes, 0);
	USEMEM(state, state->read_buffer_size * numInputTapes);

	/* End of step D2: rewind all output tapes to prepare for merging */
	for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
		LogicalTapeRewindForRead(state->tapeset, tapenum, state->read_buffer_size);

	for (;;)
	{
		/*
		 * At this point we know that tape[T] is empty.  If there's just one
		 * (real or dummy) run left on each input tape, then only one merge
		 * pass remains.  If we don't have to produce a materialized sorted
		 * tape, we can stop at this point and do the final merge on-the-fly.
		 */
		if (!state->randomAccess && !WORKER(state))
		{
			bool		allOneRun = true;

			Assert(state->tp_runs[state->tapeRange] == 0);
			for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
			{
				if (state->tp_runs[tapenum] + state->tp_dummy[tapenum] != 1)
				{
					allOneRun = false;
					break;
				}
			}
			if (allOneRun)
			{
				/* Tell logtape.c we won't be writing anymore */
				LogicalTapeSetForgetFreeSpace(state->tapeset);
				/* Initialize for the final merge pass */
				beginmerge(state);
				state->status = TSS_FINALMERGE;
				return;
			}
		}

		/* Step D5: merge runs onto tape[T] until tape[P] is empty */
		while (state->tp_runs[state->tapeRange - 1] ||
			   state->tp_dummy[state->tapeRange - 1])
		{
			bool		allDummy = true;

			for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
			{
				if (state->tp_dummy[tapenum] == 0)
				{
					allDummy = false;
					break;
				}
			}

			if (allDummy)
			{
				state->tp_dummy[state->tapeRange]++;
				for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
					state->tp_dummy[tapenum]--;
			}
			else
				mergeonerun(state);
		}

		/* Step D6: decrease level */
		if (--state->Level == 0)
			break;
		/* rewind output tape T to use as new input */
		LogicalTapeRewindForRead(state->tapeset, state->tp_tapenum[state->tapeRange],
								 state->read_buffer_size);
		/* rewind used-up input tape P, and prepare it for write pass */
		LogicalTapeRewindForWrite(state->tapeset, state->tp_tapenum[state->tapeRange - 1]);
		state->tp_runs[state->tapeRange - 1] = 0;

		/*
		 * reassign tape units per step D6; note we no longer care about A[]
		 */
		svTape = state->tp_tapenum[state->tapeRange];
		svDummy = state->tp_dummy[state->tapeRange];
		svRuns = state->tp_runs[state->tapeRange];
		for (tapenum = state->tapeRange; tapenum > 0; tapenum--)
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
	 * Done.  Knuth says that the result is on TAPE[1], but since we exited
	 * the loop without performing the last iteration of step D6, we have not
	 * rearranged the tape unit assignment, and therefore the result is on
	 * TAPE[T].  We need to do it this way so that we can freeze the final
	 * output tape while rewinding it.  The last iteration of step D6 would be
	 * a waste of cycles anyway...
	 */
	state->result_tape = state->tp_tapenum[state->tapeRange];
	if (!WORKER(state))
		LogicalTapeFreeze(state->tapeset, state->result_tape, NULL);
	else
		worker_freeze_result_tape(state);
	state->status = TSS_SORTEDONTAPE;

	/* Release the read buffers of all the other tapes, by rewinding them. */
	for (tapenum = 0; tapenum < state->maxTapes; tapenum++)
	{
		if (tapenum != state->result_tape)
			LogicalTapeRewindForWrite(state->tapeset, tapenum);
	}
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
	int			destTape = state->tp_tapenum[state->tapeRange];
	int			srcTape;

	/*
	 * Start the merge by loading one tuple from each active source tape into
	 * the heap.  We can also decrease the input run/dummy run counts.
	 */
	beginmerge(state);

	/*
	 * Execute merge by repeatedly extracting lowest tuple in heap, writing it
	 * out, and replacing it with next tuple from same tape (if there is
	 * another one).
	 */
	while (state->memtupcount > 0)
	{
		SortTuple	stup;

		/* write the tuple to destTape */
		srcTape = state->memtuples[0].srctape;
		WRITETUP(state, destTape, &state->memtuples[0]);

		/* recycle the slot of the tuple we just wrote out, for the next read */
		if (state->memtuples[0].tuple)
			RELEASE_SLAB_SLOT(state, state->memtuples[0].tuple);

		/*
		 * pull next tuple from the tape, and replace the written-out tuple in
		 * the heap with it.
		 */
		if (mergereadnext(state, srcTape, &stup))
		{
			stup.srctape = srcTape;
			tuplesort_heap_replace_top(state, &stup);
		}
		else
			tuplesort_heap_delete_top(state);
	}

	/*
	 * When the heap empties, we're done.  Write an end-of-run marker on the
	 * output tape, and increment its count of real runs.
	 */
	markrunend(state, destTape);
	state->tp_runs[state->tapeRange]++;

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d finished %d-way merge step: %s", state->worker,
			 state->activeTapes, pg_rusage_show(&state->ru_start));
#endif
}

/*
 * beginmerge - initialize for a merge pass
 *
 * We decrease the counts of real and dummy runs for each tape, and mark
 * which tapes contain active input runs in mergeactive[].  Then, fill the
 * merge heap with the first tuple from each active tape.
 */
static void
beginmerge(Tuplesortstate *state)
{
	int			activeTapes;
	int			tapenum;
	int			srcTape;

	/* Heap should be empty here */
	Assert(state->memtupcount == 0);

	/* Adjust run counts and mark the active tapes */
	memset(state->mergeactive, 0,
		   state->maxTapes * sizeof(*state->mergeactive));
	activeTapes = 0;
	for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
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
	Assert(activeTapes > 0);
	state->activeTapes = activeTapes;

	/* Load the merge heap with the first tuple from each input tape */
	for (srcTape = 0; srcTape < state->maxTapes; srcTape++)
	{
		SortTuple	tup;

		if (mergereadnext(state, srcTape, &tup))
		{
			tup.srctape = srcTape;
			tuplesort_heap_insert(state, &tup);
		}
	}
}

/*
 * mergereadnext - read next tuple from one merge input tape
 *
 * Returns false on EOF.
 */
static bool
mergereadnext(Tuplesortstate *state, int srcTape, SortTuple *stup)
{
	unsigned int tuplen;

	if (!state->mergeactive[srcTape])
		return false;			/* tape's run is already exhausted */

	/* read next tuple, if any */
	if ((tuplen = getlen(state, srcTape, true)) == 0)
	{
		state->mergeactive[srcTape] = false;
		return false;
	}
	READTUP(state, stup, srcTape, tuplen);

	return true;
}

/*
 * dumptuples - remove tuples from memtuples and write initial run to tape
 *
 * When alltuples = true, dump everything currently in memory.  (This case is
 * only used at end of input data.)
 */
static void
dumptuples(Tuplesortstate *state, bool alltuples)
{
	int			memtupwrite;
	int			i;

	/*
	 * Nothing to do if we still fit in available memory and have array slots,
	 * unless this is the final call during initial run generation.
	 */
	if (state->memtupcount < state->memtupsize && !LACKMEM(state) &&
		!alltuples)
		return;

	/*
	 * Final call might require no sorting, in rare cases where we just so
	 * happen to have previously LACKMEM()'d at the point where exactly all
	 * remaining tuples are loaded into memory, just before input was
	 * exhausted.
	 *
	 * In general, short final runs are quite possible.  Rather than allowing
	 * a special case where there was a superfluous selectnewtape() call (i.e.
	 * a call with no subsequent run actually written to destTape), we prefer
	 * to write out a 0 tuple run.
	 *
	 * mergereadnext() is prepared for 0 tuple runs, and will reliably mark
	 * the tape inactive for the merge when called from beginmerge().  This
	 * case is therefore similar to the case where mergeonerun() finds a dummy
	 * run for the tape, and so doesn't need to merge a run from the tape (or
	 * conceptually "merges" the dummy run, if you prefer).  According to
	 * Knuth, Algorithm D "isn't strictly optimal" in its method of
	 * distribution and dummy run assignment; this edge case seems very
	 * unlikely to make that appreciably worse.
	 */
	Assert(state->status == TSS_BUILDRUNS);

	/*
	 * It seems unlikely that this limit will ever be exceeded, but take no
	 * chances
	 */
	if (state->currentRun == INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot have more than %d runs for an external sort",
						INT_MAX)));

	state->currentRun++;

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d starting quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));
#endif

	/*
	 * Sort all tuples accumulated within the allowed amount of memory for
	 * this run using quicksort
	 */
	tuplesort_sort_memtuples(state);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d finished quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));
#endif

	memtupwrite = state->memtupcount;
	for (i = 0; i < memtupwrite; i++)
	{
		WRITETUP(state, state->tp_tapenum[state->destTape],
				 &state->memtuples[i]);
		state->memtupcount--;
	}

	/*
	 * Reset tuple memory.  We've freed all of the tuples that we previously
	 * allocated.  It's important to avoid fragmentation when there is a stark
	 * change in the sizes of incoming tuples.  Fragmentation due to
	 * AllocSetFree's bucketing by size class might be particularly bad if
	 * this step wasn't taken.
	 */
	MemoryContextReset(state->tuplecontext);

	markrunend(state, state->tp_tapenum[state->destTape]);
	state->tp_runs[state->destTape]++;
	state->tp_dummy[state->destTape]--; /* per Alg D step D2 */

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d finished writing run %d to tape %d: %s",
			 state->worker, state->currentRun, state->destTape,
			 pg_rusage_show(&state->ru_start));
#endif

	if (!alltuples)
		selectnewtape(state);
}

/*
 * tuplesort_rescan		- rewind and replay the scan
 */
void
tuplesort_rescan(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

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
			LogicalTapeRewindForRead(state->tapeset,
									 state->result_tape,
									 0);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_markpos	- saves current position in the merged sort file
 */
void
tuplesort_markpos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

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

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_restorepos - restores current position in merged sort file to
 *						  last saved position
 */
void
tuplesort_restorepos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeSeek(state->tapeset,
							state->result_tape,
							state->markpos_block,
							state->markpos_offset);
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_get_stats - extract summary statistics
 *
 * This can be called after tuplesort_performsort() finishes to obtain
 * printable summary information about how the sort was performed.
 */
void
tuplesort_get_stats(Tuplesortstate *state,
					TuplesortInstrumentation *stats)
{
	/*
	 * Note: it might seem we should provide both memory and disk usage for a
	 * disk-based sort.  However, the current code doesn't track memory space
	 * accurately once we have begun to return tuples to the caller (since we
	 * don't account for pfree's the caller is expected to do), so we cannot
	 * rely on availMem in a disk sort.  This does not seem worth the overhead
	 * to fix.  Is it worth creating an API for the memory context code to
	 * tell us how much is actually used in sortcontext?
	 */
	tuplesort_updatemax(state);

	if (state->isMaxSpaceDisk)
		stats->spaceType = SORT_SPACE_TYPE_DISK;
	else
		stats->spaceType = SORT_SPACE_TYPE_MEMORY;
	stats->spaceUsed = (state->maxSpace + 1023) / 1024;

	switch (state->maxSpaceStatus)
	{
		case TSS_SORTEDINMEM:
			if (state->boundUsed)
				stats->sortMethod = SORT_TYPE_TOP_N_HEAPSORT;
			else
				stats->sortMethod = SORT_TYPE_QUICKSORT;
			break;
		case TSS_SORTEDONTAPE:
			stats->sortMethod = SORT_TYPE_EXTERNAL_SORT;
			break;
		case TSS_FINALMERGE:
			stats->sortMethod = SORT_TYPE_EXTERNAL_MERGE;
			break;
		default:
			stats->sortMethod = SORT_TYPE_STILL_IN_PROGRESS;
			break;
	}
}

/*
 * Convert TuplesortMethod to a string.
 */
const char *
tuplesort_method_name(TuplesortMethod m)
{
	switch (m)
	{
		case SORT_TYPE_STILL_IN_PROGRESS:
			return "still in progress";
		case SORT_TYPE_TOP_N_HEAPSORT:
			return "top-N heapsort";
		case SORT_TYPE_QUICKSORT:
			return "quicksort";
		case SORT_TYPE_EXTERNAL_SORT:
			return "external sort";
		case SORT_TYPE_EXTERNAL_MERGE:
			return "external merge";
	}

	return "unknown";
}

/*
 * Convert TuplesortSpaceType to a string.
 */
const char *
tuplesort_space_type_name(TuplesortSpaceType t)
{
	Assert(t == SORT_SPACE_TYPE_DISK || t == SORT_SPACE_TYPE_MEMORY);
	return t == SORT_SPACE_TYPE_DISK ? "Disk" : "Memory";
}


/*
 * Heap manipulation routines, per Knuth's Algorithm 5.2.3H.
 */

/*
 * Convert the existing unordered array of SortTuples to a bounded heap,
 * discarding all but the smallest "state->bound" tuples.
 *
 * When working with a bounded heap, we want to keep the largest entry
 * at the root (array entry zero), instead of the smallest as in the normal
 * sort case.  This allows us to discard the largest entry cheaply.
 * Therefore, we temporarily reverse the sort direction.
 */
static void
make_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;
	int			i;

	Assert(state->status == TSS_INITIAL);
	Assert(state->bounded);
	Assert(tupcount >= state->bound);
	Assert(SERIAL(state));

	/* Reverse sort direction so largest entry will be at root */
	reversedirection(state);

	state->memtupcount = 0;		/* make the heap empty */
	for (i = 0; i < tupcount; i++)
	{
		if (state->memtupcount < state->bound)
		{
			/* Insert next tuple into heap */
			/* Must copy source tuple to avoid possible overwrite */
			SortTuple	stup = state->memtuples[i];

			tuplesort_heap_insert(state, &stup);
		}
		else
		{
			/*
			 * The heap is full.  Replace the largest entry with the new
			 * tuple, or just discard it, if it's larger than anything already
			 * in the heap.
			 */
			if (COMPARETUP(state, &state->memtuples[i], &state->memtuples[0]) <= 0)
			{
				free_sort_tuple(state, &state->memtuples[i]);
				CHECK_FOR_INTERRUPTS();
			}
			else
				tuplesort_heap_replace_top(state, &state->memtuples[i]);
		}
	}

	Assert(state->memtupcount == state->bound);
	state->status = TSS_BOUNDED;
}

/*
 * Convert the bounded heap to a properly-sorted array
 */
static void
sort_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;

	Assert(state->status == TSS_BOUNDED);
	Assert(state->bounded);
	Assert(tupcount == state->bound);
	Assert(SERIAL(state));

	/*
	 * We can unheapify in place because each delete-top call will remove the
	 * largest entry, which we can promptly store in the newly freed slot at
	 * the end.  Once we're down to a single-entry heap, we're done.
	 */
	while (state->memtupcount > 1)
	{
		SortTuple	stup = state->memtuples[0];

		/* this sifts-up the next-largest entry and decreases memtupcount */
		tuplesort_heap_delete_top(state);
		state->memtuples[state->memtupcount] = stup;
	}
	state->memtupcount = tupcount;

	/*
	 * Reverse sort direction back to the original state.  This is not
	 * actually necessary but seems like a good idea for tidiness.
	 */
	reversedirection(state);

	state->status = TSS_SORTEDINMEM;
	state->boundUsed = true;
}

/*
 * Sort all memtuples using specialized qsort() routines.
 *
 * Quicksort is used for small in-memory sorts, and external sort runs.
 */
static void
tuplesort_sort_memtuples(Tuplesortstate *state)
{
	Assert(!LEADER(state));

	if (state->memtupcount > 1)
	{
		/* Can we use the single-key sort function? */
		if (state->onlyKey != NULL)
			qsort_ssup(state->memtuples, state->memtupcount,
					   state->onlyKey);
		else
			qsort_tuple(state->memtuples,
						state->memtupcount,
						state->comparetup,
						state);
	}
}

/*
 * Insert a new tuple into an empty or existing heap, maintaining the
 * heap invariant.  Caller is responsible for ensuring there's room.
 *
 * Note: For some callers, tuple points to a memtuples[] entry above the
 * end of the heap.  This is safe as long as it's not immediately adjacent
 * to the end of the heap (ie, in the [memtupcount] array entry) --- if it
 * is, it might get overwritten before being moved into the heap!
 */
static void
tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple)
{
	SortTuple  *memtuples;
	int			j;

	memtuples = state->memtuples;
	Assert(state->memtupcount < state->memtupsize);

	CHECK_FOR_INTERRUPTS();

	/*
	 * Sift-up the new entry, per Knuth 5.2.3 exercise 16. Note that Knuth is
	 * using 1-based array indexes, not 0-based.
	 */
	j = state->memtupcount++;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (COMPARETUP(state, tuple, &memtuples[i]) >= 0)
			break;
		memtuples[j] = memtuples[i];
		j = i;
	}
	memtuples[j] = *tuple;
}

/*
 * Remove the tuple at state->memtuples[0] from the heap.  Decrement
 * memtupcount, and sift up to maintain the heap invariant.
 *
 * The caller has already free'd the tuple the top node points to,
 * if necessary.
 */
static void
tuplesort_heap_delete_top(Tuplesortstate *state)
{
	SortTuple  *memtuples = state->memtuples;
	SortTuple  *tuple;

	if (--state->memtupcount <= 0)
		return;

	/*
	 * Remove the last tuple in the heap, and re-insert it, by replacing the
	 * current top node with it.
	 */
	tuple = &memtuples[state->memtupcount];
	tuplesort_heap_replace_top(state, tuple);
}

/*
 * Replace the tuple at state->memtuples[0] with a new tuple.  Sift up to
 * maintain the heap invariant.
 *
 * This corresponds to Knuth's "sift-up" algorithm (Algorithm 5.2.3H,
 * Heapsort, steps H3-H8).
 */
static void
tuplesort_heap_replace_top(Tuplesortstate *state, SortTuple *tuple)
{
	SortTuple  *memtuples = state->memtuples;
	unsigned int i,
				n;

	Assert(state->memtupcount >= 1);

	CHECK_FOR_INTERRUPTS();

	/*
	 * state->memtupcount is "int", but we use "unsigned int" for i, j, n.
	 * This prevents overflow in the "2 * i + 1" calculation, since at the top
	 * of the loop we must have i < n <= INT_MAX <= UINT_MAX/2.
	 */
	n = state->memtupcount;
	i = 0;						/* i is where the "hole" is */
	for (;;)
	{
		unsigned int j = 2 * i + 1;

		if (j >= n)
			break;
		if (j + 1 < n &&
			COMPARETUP(state, &memtuples[j], &memtuples[j + 1]) > 0)
			j++;
		if (COMPARETUP(state, tuple, &memtuples[j]) <= 0)
			break;
		memtuples[i] = memtuples[j];
		i = j;
	}
	memtuples[i] = *tuple;
}

/*
 * Function to reverse the sort direction from its current state
 *
 * It is not safe to call this when performing hash tuplesorts
 */
static void
reversedirection(Tuplesortstate *state)
{
	SortSupport sortKey = state->sortKeys;
	int			nkey;

	for (nkey = 0; nkey < state->nKeys; nkey++, sortKey++)
	{
		sortKey->ssup_reverse = !sortKey->ssup_reverse;
		sortKey->ssup_nulls_first = !sortKey->ssup_nulls_first;
	}
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplesortstate *state, int tapenum, bool eofOK)
{
	unsigned int len;

	if (LogicalTapeRead(state->tapeset, tapenum,
						&len, sizeof(len)) != sizeof(len))
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
 * Get memory for tuple from within READTUP() routine.
 *
 * We use next free slot from the slab allocator, or palloc() if the tuple
 * is too large for that.
 */
static void *
readtup_alloc(Tuplesortstate *state, Size tuplen)
{
	SlabSlot   *buf;

	/*
	 * We pre-allocate enough slots in the slab arena that we should never run
	 * out.
	 */
	Assert(state->slabFreeHead);

	if (tuplen > SLAB_SLOT_SIZE || !state->slabFreeHead)
		return MemoryContextAlloc(state->sortcontext, tuplen);
	else
	{
		buf = state->slabFreeHead;
		/* Reuse this slot */
		state->slabFreeHead = buf->nextfree;

		return buf;
	}
}


/*
 * Routines specialized for HeapTuple (actually MinimalTuple) case
 */

static int
comparetup_heap(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	SortSupport sortKey = state->sortKeys;
	HeapTupleData ltup;
	HeapTupleData rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	AttrNumber	attno;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	ltup.t_len = ((MinimalTuple) a->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	ltup.t_data = (HeapTupleHeader) ((char *) a->tuple - MINIMAL_TUPLE_OFFSET);
	rtup.t_len = ((MinimalTuple) b->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	rtup.t_data = (HeapTupleHeader) ((char *) b->tuple - MINIMAL_TUPLE_OFFSET);
	tupDesc = state->tupDesc;

	if (sortKey->abbrev_converter)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	sortKey++;
	for (nkey = 1; nkey < state->nKeys; nkey++, sortKey++)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;
	}

	return 0;
}

static void
copytup_heap(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	/*
	 * We expect the passed "tup" to be a TupleTableSlot, and form a
	 * MinimalTuple using the exported interface for that.
	 */
	TupleTableSlot *slot = (TupleTableSlot *) tup;
	Datum		original;
	MinimalTuple tuple;
	HeapTupleData htup;
	MemoryContext oldcontext = MemoryContextSwitchTo(state->tuplecontext);

	/* copy the tuple into sort storage */
	tuple = ExecCopySlotMinimalTuple(slot);
	stup->tuple = (void *) tuple;
	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	original = heap_getattr(&htup,
							state->sortKeys[0].ssup_attno,
							state->tupDesc,
							&stup->isnull1);

	MemoryContextSwitchTo(oldcontext);

	if (!state->sortKeys->abbrev_converter || stup->isnull1)
	{
		/*
		 * Store ordinary Datum representation, or NULL value.  If there is a
		 * converter it won't expect NULL values, and cost model is not
		 * required to account for NULL, so in that case we avoid calling
		 * converter and just set datum1 to zeroed representation (to be
		 * consistent, and to support cheap inequality tests for NULL
		 * abbreviated keys).
		 */
		stup->datum1 = original;
	}
	else if (!consider_abort_common(state))
	{
		/* Store abbreviated key representation */
		stup->datum1 = state->sortKeys->abbrev_converter(original,
														 state->sortKeys);
	}
	else
	{
		/* Abort abbreviation */
		int			i;

		stup->datum1 = original;

		/*
		 * Set state to be consistent with never trying abbreviation.
		 *
		 * Alter datum1 representation in already-copied tuples, so as to
		 * ensure a consistent representation (current tuple was just
		 * handled).  It does not matter if some dumped tuples are already
		 * sorted on tape, since serialized tuples lack abbreviated keys
		 * (TSS_BUILDRUNS state prevents control reaching here in any case).
		 */
		for (i = 0; i < state->memtupcount; i++)
		{
			SortTuple  *mtup = &state->memtuples[i];

			htup.t_len = ((MinimalTuple) mtup->tuple)->t_len +
				MINIMAL_TUPLE_OFFSET;
			htup.t_data = (HeapTupleHeader) ((char *) mtup->tuple -
											 MINIMAL_TUPLE_OFFSET);

			mtup->datum1 = heap_getattr(&htup,
										state->sortKeys[0].ssup_attno,
										state->tupDesc,
										&mtup->isnull1);
		}
	}
}

static void
writetup_heap(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	MinimalTuple tuple = (MinimalTuple) stup->tuple;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tupbody, tupbodylen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	if (!state->slabAllocatorUsed)
	{
		FREEMEM(state, GetMemoryChunkSpace(tuple));
		heap_free_minimal_tuple(tuple);
	}
}

static void
readtup_heap(Tuplesortstate *state, SortTuple *stup,
			 int tapenum, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) readtup_alloc(state, tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	HeapTupleData htup;

	/* read in the tuple proper */
	tuple->t_len = tuplen;
	LogicalTapeReadExact(state->tapeset, tapenum,
						 tupbody, tupbodylen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup->datum1 = heap_getattr(&htup,
								state->sortKeys[0].ssup_attno,
								state->tupDesc,
								&stup->isnull1);
}

/*
 * Routines specialized for the CLUSTER case (HeapTuple data, with
 * comparisons per a btree index definition)
 */

static int
comparetup_cluster(const SortTuple *a, const SortTuple *b,
				   Tuplesortstate *state)
{
	SortSupport sortKey = state->sortKeys;
	HeapTuple	ltup;
	HeapTuple	rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;
	AttrNumber	leading = state->indexInfo->ii_IndexAttrNumbers[0];

	/* Be prepared to compare additional sort keys */
	ltup = (HeapTuple) a->tuple;
	rtup = (HeapTuple) b->tuple;
	tupDesc = state->tupDesc;

	/* Compare the leading sort key, if it's simple */
	if (leading != 0)
	{
		compare = ApplySortComparator(a->datum1, a->isnull1,
									  b->datum1, b->isnull1,
									  sortKey);
		if (compare != 0)
			return compare;

		if (sortKey->abbrev_converter)
		{
			datum1 = heap_getattr(ltup, leading, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, leading, tupDesc, &isnull2);

			compare = ApplySortAbbrevFullComparator(datum1, isnull1,
													datum2, isnull2,
													sortKey);
		}
		if (compare != 0 || state->nKeys == 1)
			return compare;
		/* Compare additional columns the hard way */
		sortKey++;
		nkey = 1;
	}
	else
	{
		/* Must compare all keys the hard way */
		nkey = 0;
	}

	if (state->indexInfo->ii_Expressions == NULL)
	{
		/* If not expression index, just compare the proper heap attrs */

		for (; nkey < state->nKeys; nkey++, sortKey++)
		{
			AttrNumber	attno = state->indexInfo->ii_IndexAttrNumbers[nkey];

			datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

			compare = ApplySortComparator(datum1, isnull1,
										  datum2, isnull2,
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}
	else
	{
		/*
		 * In the expression index case, compute the whole index tuple and
		 * then compare values.  It would perhaps be faster to compute only as
		 * many columns as we need to compare, but that would require
		 * duplicating all the logic in FormIndexDatum.
		 */
		Datum		l_index_values[INDEX_MAX_KEYS];
		bool		l_index_isnull[INDEX_MAX_KEYS];
		Datum		r_index_values[INDEX_MAX_KEYS];
		bool		r_index_isnull[INDEX_MAX_KEYS];
		TupleTableSlot *ecxt_scantuple;

		/* Reset context each time to prevent memory leakage */
		ResetPerTupleExprContext(state->estate);

		ecxt_scantuple = GetPerTupleExprContext(state->estate)->ecxt_scantuple;

		ExecStoreHeapTuple(ltup, ecxt_scantuple, false);
		FormIndexDatum(state->indexInfo, ecxt_scantuple, state->estate,
					   l_index_values, l_index_isnull);

		ExecStoreHeapTuple(rtup, ecxt_scantuple, false);
		FormIndexDatum(state->indexInfo, ecxt_scantuple, state->estate,
					   r_index_values, r_index_isnull);

		for (; nkey < state->nKeys; nkey++, sortKey++)
		{
			compare = ApplySortComparator(l_index_values[nkey],
										  l_index_isnull[nkey],
										  r_index_values[nkey],
										  r_index_isnull[nkey],
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}

	return 0;
}

static void
copytup_cluster(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;
	Datum		original;
	MemoryContext oldcontext = MemoryContextSwitchTo(state->tuplecontext);

	/* copy the tuple into sort storage */
	tuple = heap_copytuple(tuple);
	stup->tuple = (void *) tuple;
	USEMEM(state, GetMemoryChunkSpace(tuple));

	MemoryContextSwitchTo(oldcontext);

	/*
	 * set up first-column key value, and potentially abbreviate, if it's a
	 * simple column
	 */
	if (state->indexInfo->ii_IndexAttrNumbers[0] == 0)
		return;

	original = heap_getattr(tuple,
							state->indexInfo->ii_IndexAttrNumbers[0],
							state->tupDesc,
							&stup->isnull1);

	if (!state->sortKeys->abbrev_converter || stup->isnull1)
	{
		/*
		 * Store ordinary Datum representation, or NULL value.  If there is a
		 * converter it won't expect NULL values, and cost model is not
		 * required to account for NULL, so in that case we avoid calling
		 * converter and just set datum1 to zeroed representation (to be
		 * consistent, and to support cheap inequality tests for NULL
		 * abbreviated keys).
		 */
		stup->datum1 = original;
	}
	else if (!consider_abort_common(state))
	{
		/* Store abbreviated key representation */
		stup->datum1 = state->sortKeys->abbrev_converter(original,
														 state->sortKeys);
	}
	else
	{
		/* Abort abbreviation */
		int			i;

		stup->datum1 = original;

		/*
		 * Set state to be consistent with never trying abbreviation.
		 *
		 * Alter datum1 representation in already-copied tuples, so as to
		 * ensure a consistent representation (current tuple was just
		 * handled).  It does not matter if some dumped tuples are already
		 * sorted on tape, since serialized tuples lack abbreviated keys
		 * (TSS_BUILDRUNS state prevents control reaching here in any case).
		 */
		for (i = 0; i < state->memtupcount; i++)
		{
			SortTuple  *mtup = &state->memtuples[i];

			tuple = (HeapTuple) mtup->tuple;
			mtup->datum1 = heap_getattr(tuple,
										state->indexInfo->ii_IndexAttrNumbers[0],
										state->tupDesc,
										&mtup->isnull1);
		}
	}
}

static void
writetup_cluster(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	HeapTuple	tuple = (HeapTuple) stup->tuple;
	unsigned int tuplen = tuple->t_len + sizeof(ItemPointerData) + sizeof(int);

	/* We need to store t_self, but not other fields of HeapTupleData */
	LogicalTapeWrite(state->tapeset, tapenum,
					 &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 &tuple->t_self, sizeof(ItemPointerData));
	LogicalTapeWrite(state->tapeset, tapenum,
					 tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 &tuplen, sizeof(tuplen));

	if (!state->slabAllocatorUsed)
	{
		FREEMEM(state, GetMemoryChunkSpace(tuple));
		heap_freetuple(tuple);
	}
}

static void
readtup_cluster(Tuplesortstate *state, SortTuple *stup,
				int tapenum, unsigned int tuplen)
{
	unsigned int t_len = tuplen - sizeof(ItemPointerData) - sizeof(int);
	HeapTuple	tuple = (HeapTuple) readtup_alloc(state,
												  t_len + HEAPTUPLESIZE);

	/* Reconstruct the HeapTupleData header */
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);
	tuple->t_len = t_len;
	LogicalTapeReadExact(state->tapeset, tapenum,
						 &tuple->t_self, sizeof(ItemPointerData));
	/* We don't currently bother to reconstruct t_tableOid */
	tuple->t_tableOid = InvalidOid;
	/* Read in the tuple body */
	LogicalTapeReadExact(state->tapeset, tapenum,
						 tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value, if it's a simple column */
	if (state->indexInfo->ii_IndexAttrNumbers[0] != 0)
		stup->datum1 = heap_getattr(tuple,
									state->indexInfo->ii_IndexAttrNumbers[0],
									state->tupDesc,
									&stup->isnull1);
}

/*
 * Routines specialized for IndexTuple case
 *
 * The btree and hash cases require separate comparison functions, but the
 * IndexTuple representation is the same so the copy/write/read support
 * functions can be shared.
 */

static int
comparetup_index_btree(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state)
{
	/*
	 * This is similar to comparetup_heap(), but expects index tuples.  There
	 * is also special handling for enforcing uniqueness, and special
	 * treatment for equal keys at the end.
	 */
	SortSupport sortKey = state->sortKeys;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	int			keysz;
	TupleDesc	tupDes;
	bool		equal_hasnull = false;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;
	keysz = state->nKeys;
	tupDes = RelationGetDescr(state->indexRel);

	if (sortKey->abbrev_converter)
	{
		datum1 = index_getattr(tuple1, 1, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, 1, tupDes, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	/* they are equal, so we only need to examine one null flag */
	if (a->isnull1)
		equal_hasnull = true;

	sortKey++;
	for (nkey = 2; nkey <= keysz; nkey++, sortKey++)
	{
		datum1 = index_getattr(tuple1, nkey, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, nkey, tupDes, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;		/* done when we find unequal attributes */

		/* they are equal, so we only need to examine one null flag */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * If btree has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field).
	 *
	 * It is sufficient to make the test here, because if two tuples are equal
	 * they *must* get compared at some stage of the sort --- otherwise the
	 * sort algorithm wouldn't have checked whether one must appear before the
	 * other.
	 */
	if (state->enforceUnique && !equal_hasnull)
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		/*
		 * Some rather brain-dead implementations of qsort (such as the one in
		 * QNX 4) will sometimes call the comparison routine to compare a
		 * value to itself, but we always use our own implementation, which
		 * does not.
		 */
		Assert(tuple1 != tuple2);

		index_deform_tuple(tuple1, tupDes, values, isnull);

		key_desc = BuildIndexValueDescription(state->indexRel, values, isnull);

		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(state->indexRel)),
				 key_desc ? errdetail("Key %s is duplicated.", key_desc) :
				 errdetail("Duplicate keys exist."),
				 errtableconstraint(state->heapRel,
									RelationGetRelationName(state->indexRel))));
	}

	/*
	 * If key values are equal, we sort on ItemPointer.  This is required for
	 * btree indexes, since heap TID is treated as an implicit last key
	 * attribute in order to ensure that all keys in the index are physically
	 * unique.
	 */
	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer values should never be equal */
	Assert(false);

	return 0;
}

static int
comparetup_index_hash(const SortTuple *a, const SortTuple *b,
					  Tuplesortstate *state)
{
	Bucket		bucket1;
	Bucket		bucket2;
	IndexTuple	tuple1;
	IndexTuple	tuple2;

	/*
	 * Fetch hash keys and mask off bits we don't want to sort by. We know
	 * that the first column of the index tuple is the hash key.
	 */
	Assert(!a->isnull1);
	bucket1 = _hash_hashkey2bucket(DatumGetUInt32(a->datum1),
								   state->max_buckets, state->high_mask,
								   state->low_mask);
	Assert(!b->isnull1);
	bucket2 = _hash_hashkey2bucket(DatumGetUInt32(b->datum1),
								   state->max_buckets, state->high_mask,
								   state->low_mask);
	if (bucket1 > bucket2)
		return 1;
	else if (bucket1 < bucket2)
		return -1;

	/*
	 * If hash values are equal, we sort on ItemPointer.  This does not affect
	 * validity of the finished index, but it may be useful to have index
	 * scans in physical order.
	 */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer values should never be equal */
	Assert(false);

	return 0;
}

static void
copytup_index(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	/* Not currently needed */
	elog(ERROR, "copytup_index() should not be called");
}

static void
writetup_index(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	IndexTuple	tuple = (IndexTuple) stup->tuple;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tuple, IndexTupleSize(tuple));
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	if (!state->slabAllocatorUsed)
	{
		FREEMEM(state, GetMemoryChunkSpace(tuple));
		pfree(tuple);
	}
}

static void
readtup_index(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) readtup_alloc(state, tuplen);

	LogicalTapeReadExact(state->tapeset, tapenum,
						 tuple, tuplen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(state->indexRel),
								 &stup->isnull1);
}

/*
 * Routines specialized for DatumTuple case
 */

static int
comparetup_datum(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  state->sortKeys);
	if (compare != 0)
		return compare;

	/* if we have abbreviations, then "tuple" has the original value */

	if (state->sortKeys->abbrev_converter)
		compare = ApplySortAbbrevFullComparator(PointerGetDatum(a->tuple), a->isnull1,
												PointerGetDatum(b->tuple), b->isnull1,
												state->sortKeys);

	return compare;
}

static void
copytup_datum(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	/* Not currently needed */
	elog(ERROR, "copytup_datum() should not be called");
}

static void
writetup_datum(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	void	   *waddr;
	unsigned int tuplen;
	unsigned int writtenlen;

	if (stup->isnull1)
	{
		waddr = NULL;
		tuplen = 0;
	}
	else if (!state->tuples)
	{
		waddr = &stup->datum1;
		tuplen = sizeof(Datum);
	}
	else
	{
		waddr = stup->tuple;
		tuplen = datumGetSize(PointerGetDatum(stup->tuple), false, state->datumTypeLen);
		Assert(tuplen != 0);
	}

	writtenlen = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &writtenlen, sizeof(writtenlen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 waddr, tuplen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &writtenlen, sizeof(writtenlen));

	if (!state->slabAllocatorUsed && stup->tuple)
	{
		FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
		pfree(stup->tuple);
	}
}

static void
readtup_datum(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);

	if (tuplen == 0)
	{
		/* it's NULL */
		stup->datum1 = (Datum) 0;
		stup->isnull1 = true;
		stup->tuple = NULL;
	}
	else if (!state->tuples)
	{
		Assert(tuplen == sizeof(Datum));
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &stup->datum1, tuplen);
		stup->isnull1 = false;
		stup->tuple = NULL;
	}
	else
	{
		void	   *raddr = readtup_alloc(state, tuplen);

		LogicalTapeReadExact(state->tapeset, tapenum,
							 raddr, tuplen);
		stup->datum1 = PointerGetDatum(raddr);
		stup->isnull1 = false;
		stup->tuple = raddr;
	}

	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
}

/*
 * Parallel sort routines
 */

/*
 * tuplesort_estimate_shared - estimate required shared memory allocation
 *
 * nWorkers is an estimate of the number of workers (it's the number that
 * will be requested).
 */
Size
tuplesort_estimate_shared(int nWorkers)
{
	Size		tapesSize;

	Assert(nWorkers > 0);

	/* Make sure that BufFile shared state is MAXALIGN'd */
	tapesSize = mul_size(sizeof(TapeShare), nWorkers);
	tapesSize = MAXALIGN(add_size(tapesSize, offsetof(Sharedsort, tapes)));

	return tapesSize;
}

/*
 * tuplesort_initialize_shared - initialize shared tuplesort state
 *
 * Must be called from leader process before workers are launched, to
 * establish state needed up-front for worker tuplesortstates.  nWorkers
 * should match the argument passed to tuplesort_estimate_shared().
 */
void
tuplesort_initialize_shared(Sharedsort *shared, int nWorkers, dsm_segment *seg)
{
	int			i;

	Assert(nWorkers > 0);

	SpinLockInit(&shared->mutex);
	shared->currentWorker = 0;
	shared->workersFinished = 0;
	SharedFileSetInit(&shared->fileset, seg);
	shared->nTapes = nWorkers;
	for (i = 0; i < nWorkers; i++)
	{
		shared->tapes[i].firstblocknumber = 0L;
	}
}

/*
 * tuplesort_attach_shared - attach to shared tuplesort state
 *
 * Must be called by all worker processes.
 */
void
tuplesort_attach_shared(Sharedsort *shared, dsm_segment *seg)
{
	/* Attach to SharedFileSet */
	SharedFileSetAttach(&shared->fileset, seg);
}

/*
 * worker_get_identifier - Assign and return ordinal identifier for worker
 *
 * The order in which these are assigned is not well defined, and should not
 * matter; worker numbers across parallel sort participants need only be
 * distinct and gapless.  logtape.c requires this.
 *
 * Note that the identifiers assigned from here have no relation to
 * ParallelWorkerNumber number, to avoid making any assumption about
 * caller's requirements.  However, we do follow the ParallelWorkerNumber
 * convention of representing a non-worker with worker number -1.  This
 * includes the leader, as well as serial Tuplesort processes.
 */
static int
worker_get_identifier(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	int			worker;

	Assert(WORKER(state));

	SpinLockAcquire(&shared->mutex);
	worker = shared->currentWorker++;
	SpinLockRelease(&shared->mutex);

	return worker;
}

/*
 * worker_freeze_result_tape - freeze worker's result tape for leader
 *
 * This is called by workers just after the result tape has been determined,
 * instead of calling LogicalTapeFreeze() directly.  They do so because
 * workers require a few additional steps over similar serial
 * TSS_SORTEDONTAPE external sort cases, which also happen here.  The extra
 * steps are around freeing now unneeded resources, and representing to
 * leader that worker's input run is available for its merge.
 *
 * There should only be one final output run for each worker, which consists
 * of all tuples that were originally input into worker.
 */
static void
worker_freeze_result_tape(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	TapeShare	output;

	Assert(WORKER(state));
	Assert(state->result_tape != -1);
	Assert(state->memtupcount == 0);

	/*
	 * Free most remaining memory, in case caller is sensitive to our holding
	 * on to it.  memtuples may not be a tiny merge heap at this point.
	 */
	pfree(state->memtuples);
	/* Be tidy */
	state->memtuples = NULL;
	state->memtupsize = 0;

	/*
	 * Parallel worker requires result tape metadata, which is to be stored in
	 * shared memory for leader
	 */
	LogicalTapeFreeze(state->tapeset, state->result_tape, &output);

	/* Store properties of output tape, and update finished worker count */
	SpinLockAcquire(&shared->mutex);
	shared->tapes[state->worker] = output;
	shared->workersFinished++;
	SpinLockRelease(&shared->mutex);
}

/*
 * worker_nomergeruns - dump memtuples in worker, without merging
 *
 * This called as an alternative to mergeruns() with a worker when no
 * merging is required.
 */
static void
worker_nomergeruns(Tuplesortstate *state)
{
	Assert(WORKER(state));
	Assert(state->result_tape == -1);

	state->result_tape = state->tp_tapenum[state->destTape];
	worker_freeze_result_tape(state);
}

/*
 * leader_takeover_tapes - create tapeset for leader from worker tapes
 *
 * So far, leader Tuplesortstate has performed no actual sorting.  By now, all
 * sorting has occurred in workers, all of which must have already returned
 * from tuplesort_performsort().
 *
 * When this returns, leader process is left in a state that is virtually
 * indistinguishable from it having generated runs as a serial external sort
 * might have.
 */
static void
leader_takeover_tapes(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	int			nParticipants = state->nParticipants;
	int			workersFinished;
	int			j;

	Assert(LEADER(state));
	Assert(nParticipants >= 1);

	SpinLockAcquire(&shared->mutex);
	workersFinished = shared->workersFinished;
	SpinLockRelease(&shared->mutex);

	if (nParticipants != workersFinished)
		elog(ERROR, "cannot take over tapes before all workers finish");

	/*
	 * Create the tapeset from worker tapes, including a leader-owned tape at
	 * the end.  Parallel workers are far more expensive than logical tapes,
	 * so the number of tapes allocated here should never be excessive.
	 *
	 * We still have a leader tape, though it's not possible to write to it
	 * due to restrictions in the shared fileset infrastructure used by
	 * logtape.c.  It will never be written to in practice because
	 * randomAccess is disallowed for parallel sorts.
	 */
	inittapestate(state, nParticipants + 1);
	state->tapeset = LogicalTapeSetCreate(nParticipants + 1, false,
										  shared->tapes, &shared->fileset,
										  state->worker);

	/* mergeruns() relies on currentRun for # of runs (in one-pass cases) */
	state->currentRun = nParticipants;

	/*
	 * Initialize variables of Algorithm D to be consistent with runs from
	 * workers having been generated in the leader.
	 *
	 * There will always be exactly 1 run per worker, and exactly one input
	 * tape per run, because workers always output exactly 1 run, even when
	 * there were no input tuples for workers to sort.
	 */
	for (j = 0; j < state->maxTapes; j++)
	{
		/* One real run; no dummy runs for worker tapes */
		state->tp_fib[j] = 1;
		state->tp_runs[j] = 1;
		state->tp_dummy[j] = 0;
		state->tp_tapenum[j] = j;
	}
	/* Leader tape gets one dummy run, and no real runs */
	state->tp_fib[state->tapeRange] = 0;
	state->tp_runs[state->tapeRange] = 0;
	state->tp_dummy[state->tapeRange] = 1;

	state->Level = 1;
	state->destTape = 0;

	state->status = TSS_BUILDRUNS;
}

/*
 * Convenience routine to free a tuple previously loaded into sort memory
 */
static void
free_sort_tuple(Tuplesortstate *state, SortTuple *stup)
{
	FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
	pfree(stup->tuple);
}
