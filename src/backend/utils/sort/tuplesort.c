/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module provides a generalized facility for tuple sorting, which can be
 * applied to different kinds of sortable objects.  Implementation of
 * the particular sorting variants is given in tuplesortvariants.c.
 * This module works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().  Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * See Knuth, volume 3, for more than you want to know about external
 * sorting algorithms.  The algorithm we use is a balanced k-way merge.
 * Before PostgreSQL 15, we used the polyphase merge algorithm (Knuth's
 * Algorithm 5.4.2D), but with modern hardware, a straightforward balanced
 * merge is better.  Knuth is assuming that tape drives are expensive
 * beasts, and in particular that there will always be many more runs than
 * tape drives.  The polyphase merge algorithm was good at keeping all the
 * tape drives busy, but in our implementation a "tape drive" doesn't cost
 * much more than a few Kb of memory buffers, so we can afford to have
 * lots of them.  In particular, if we can have as many tape drives as
 * sorted runs, we can eliminate any repeated I/O at all.
 *
 * Historically, we divided the input into sorted runs using replacement
 * selection, in the form of a priority tree implemented as a heap
 * (essentially Knuth's Algorithm 5.2.3H), but now we always use quicksort
 * for run generation.
 *
 * The approximate amount of memory allowed for any one sort operation
 * is specified in kilobytes by the caller (most pass work_mem).  Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded workMem.  If we reach the end of the input without
 * exceeding workMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * workMem, we begin to emit tuples into sorted runs in temporary tapes.
 * When tuples are dumped in batch after quicksorting, we begin a new run
 * with a new output tape.  If we reach the max number of tapes, we write
 * subsequent runs on the existing tapes in a round-robin fashion.  We will
 * need multiple merge passes to finish the merge in that case.  After the
 * end of the input is reached, we dump out remaining tuples in memory into
 * a final run, then merge the runs.
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
 * In the current code we determine the number of input tapes M on the basis
 * of workMem: we want workMem/M to be large enough that we read a fair
 * amount of data each time we read from a tape, so as to maintain the
 * locality of access described above.  Nonetheless, with large workMem we
 * can have many tapes.  The logical "tapes" are implemented by logtape.c,
 * which avoids space wastage by recycling disk space as soon as each block
 * is read from its "tape".
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.  When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
 * to one run per logical tape.  The final merge is then performed
 * on-the-fly as the caller repeatedly calls tuplesort_getXXX; this
 * saves one cycle of writing all the data out to disk and reading it in.
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
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "commands/tablespace.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/tuplesort.h"

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
bool		trace_sort = false;

#ifdef DEBUG_BOUNDED_SORT
bool		optimize_bounded_sort = true;
#endif


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
	TSS_FINALMERGE,				/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * Parameters for calculation of number of tapes to use --- see inittapes()
 * and tuplesort_merge_order().
 *
 * In this calculation we assume that each tape will cost us about 1 blocks
 * worth of buffer space.  This ignores the overhead of all the other data
 * structures needed for each tape, but it's probably close enough.
 *
 * MERGE_BUFFER_SIZE is how much buffer space we'd like to allocate for each
 * input tape, for pre-reading (see discussion at top of file).  This is *in
 * addition to* the 1 block already included in TAPE_BUFFER_OVERHEAD.
 */
#define MINORDER		6		/* minimum merge order */
#define MAXORDER		500		/* maximum merge order */
#define TAPE_BUFFER_OVERHEAD		BLCKSZ
#define MERGE_BUFFER_SIZE			(BLCKSZ * 32)


/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate
{
	TuplesortPublic base;
	TupSortStatus status;		/* enumerated value as shown above */
	bool		bounded;		/* did caller specify a maximum number of
								 * tuples to return? */
	bool		boundUsed;		/* true if we made use of a bounded heap */
	int			bound;			/* if bounded, the maximum number of tuples */
	int64		tupleMem;		/* memory consumed by individual tuples.
								 * storing this separately from what we track
								 * in availMem allows us to subtract the
								 * memory consumed by all tuples when dumping
								 * tuples to tape */
	int64		availMem;		/* remaining memory available, in bytes */
	int64		allowedMem;		/* total memory allowed, in bytes */
	int			maxTapes;		/* max number of input tapes to merge in each
								 * pass */
	int64		maxSpace;		/* maximum amount of space occupied among sort
								 * of groups, either in-memory or on-disk */
	bool		isMaxSpaceDisk; /* true when maxSpace is value for on-disk
								 * space, false when it's value for in-memory
								 * space */
	TupSortStatus maxSpaceStatus;	/* sort status when maxSpace was reached */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

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

	/* Memory used for input and output tape buffers. */
	size_t		tape_buffer_mem;

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
	 * Logical tapes, for merging.
	 *
	 * The initial runs are written in the output tapes.  In each merge pass,
	 * the output tapes of the previous pass become the input tapes, and new
	 * output tapes are created as needed.  When nInputTapes equals
	 * nInputRuns, there is only one merge pass left.
	 */
	LogicalTape **inputTapes;
	int			nInputTapes;
	int			nInputRuns;

	LogicalTape **outputTapes;
	int			nOutputTapes;
	int			nOutputRuns;

	LogicalTape *destTape;		/* current output tape */

	/*
	 * These variables are used after completion of sorting to keep track of
	 * the next tuple to return.  (In the tape case, the tape's current read
	 * position is also critical state.)
	 */
	LogicalTape *result_tape;	/* actual tape of finished output */
	int			current;		/* array index (only used if SORTEDINMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	int64		markpos_block;	/* tape block# (only used if SORTEDONTAPE) */
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
	 * finish a run that the leader needs to merge.  Typically includes a
	 * worker state held by the leader process itself.  Set in the leader
	 * Tuplesortstate only.
	 */
	int			worker;
	Sharedsort *shared;
	int			nParticipants;

	/*
	 * Additional state for managing "abbreviated key" sortsupport routines
	 * (which currently may be used by all cases except the hash index case).
	 * Tracks the intervals at which the optimization's effectiveness is
	 * tested.
	 */
	int64		abbrevNext;		/* Tuple # at which to next check
								 * applicability */

	/*
	 * Resource snapshot for time of sort start.
	 */
	PGRUsage	ru_start;
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

#define REMOVEABBREV(state,stup,count)	((*(state)->base.removeabbrev) (state, stup, count))
#define COMPARETUP(state,a,b)	((*(state)->base.comparetup) (a, b, state))
#define WRITETUP(state,tape,stup)	((*(state)->base.writetup) (state, tape, stup))
#define READTUP(state,stup,tape,len) ((*(state)->base.readtup) (state, stup, tape, len))
#define FREESTATE(state)	((state)->base.freestate ? (*(state)->base.freestate) (state) : (void) 0)
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
 * If state->sortopt contains TUPLESORT_RANDOMACCESS, then the stored
 * representation of the tuple must be followed by another "unsigned int" that
 * is a copy of the length --- so the total tape space used is actually
 * sizeof(unsigned int) more than the stored length value.  This allows
 * read-backwards.  When the random access flag was not specified, the
 * write/read routines may omit the extra length word.
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
 * readtup routines use the slab allocator (they cannot use
 * the reset context because it gets deleted at the point that merging
 * begins).
 */


static void tuplesort_begin_batch(Tuplesortstate *state);
static bool consider_abort_common(Tuplesortstate *state);
static void inittapes(Tuplesortstate *state, bool mergeruns);
static void inittapestate(Tuplesortstate *state, int maxTapes);
static void selectnewtape(Tuplesortstate *state);
static void init_slab_allocator(Tuplesortstate *state, int numSlots);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static bool mergereadnext(Tuplesortstate *state, LogicalTape *srcTape, SortTuple *stup);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void make_bounded_heap(Tuplesortstate *state);
static void sort_bounded_heap(Tuplesortstate *state);
static void tuplesort_sort_memtuples(Tuplesortstate *state);
static void tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_replace_top(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_delete_top(Tuplesortstate *state);
static void reversedirection(Tuplesortstate *state);
static unsigned int getlen(LogicalTape *tape, bool eofOK);
static void markrunend(LogicalTape *tape);
static int	worker_get_identifier(Tuplesortstate *state);
static void worker_freeze_result_tape(Tuplesortstate *state);
static void worker_nomergeruns(Tuplesortstate *state);
static void leader_takeover_tapes(Tuplesortstate *state);
static void free_sort_tuple(Tuplesortstate *state, SortTuple *stup);
static void tuplesort_free(Tuplesortstate *state);
static void tuplesort_updatemax(Tuplesortstate *state);

/*
 * Specialized comparators that we can inline into specialized sorts.  The goal
 * is to try to sort two tuples without having to follow the pointers to the
 * comparator or the tuple.
 *
 * XXX: For now, there is no specialization for cases where datum1 is
 * authoritative and we don't even need to fall back to a callback at all (that
 * would be true for types like int4/int8/timestamp/date, but not true for
 * abbreviations of text or multi-key sorts.  There could be!  Is it worth it?
 */

/* Used if first key's comparator is ssup_datum_unsigned_cmp */
static pg_attribute_always_inline int
qsort_tuple_unsigned_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplyUnsignedSortComparator(a->datum1, a->isnull1,
										  b->datum1, b->isnull1,
										  &state->base.sortKeys[0]);
	if (compare != 0)
		return compare;

	/*
	 * No need to waste effort calling the tiebreak function when there are no
	 * other keys to sort on.
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup_tiebreak(a, b, state);
}

#if SIZEOF_DATUM >= 8
/* Used if first key's comparator is ssup_datum_signed_cmp */
static pg_attribute_always_inline int
qsort_tuple_signed_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplySignedSortComparator(a->datum1, a->isnull1,
										b->datum1, b->isnull1,
										&state->base.sortKeys[0]);

	if (compare != 0)
		return compare;

	/*
	 * No need to waste effort calling the tiebreak function when there are no
	 * other keys to sort on.
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup_tiebreak(a, b, state);
}
#endif

/* Used if first key's comparator is ssup_datum_int32_cmp */
static pg_attribute_always_inline int
qsort_tuple_int32_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplyInt32SortComparator(a->datum1, a->isnull1,
									   b->datum1, b->isnull1,
									   &state->base.sortKeys[0]);

	if (compare != 0)
		return compare;

	/*
	 * No need to waste effort calling the tiebreak function when there are no
	 * other keys to sort on.
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup_tiebreak(a, b, state);
}

/*
 * Special versions of qsort just for SortTuple objects.  qsort_tuple() sorts
 * any variant of SortTuples, using the appropriate comparetup function.
 * qsort_ssup() is specialized for the case where the comparetup function
 * reduces to ApplySortComparator(), that is single-key MinimalTuple sorts
 * and Datum sorts.  qsort_tuple_{unsigned,signed,int32} are specialized for
 * common comparison functions on pass-by-value leading datums.
 */

#define ST_SORT qsort_tuple_unsigned
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_unsigned_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

#if SIZEOF_DATUM >= 8
#define ST_SORT qsort_tuple_signed
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_signed_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"
#endif

#define ST_SORT qsort_tuple_int32
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_int32_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT qsort_tuple
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE_RUNTIME_POINTER
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT qsort_ssup
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, ssup) \
	ApplySortComparator((a)->datum1, (a)->isnull1, \
						(b)->datum1, (b)->isnull1, (ssup))
#define ST_COMPARE_ARG_TYPE SortSupportData
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

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
 * other values.)  Each variant also has a sortopt which is a bitmask of
 * sort options.  See TUPLESORT_* definitions in tuplesort.h
 */

Tuplesortstate *
tuplesort_begin_common(int workMem, SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state;
	MemoryContext maincontext;
	MemoryContext sortcontext;
	MemoryContext oldcontext;

	/* See leader_takeover_tapes() remarks on random access support */
	if (coordinate && (sortopt & TUPLESORT_RANDOMACCESS))
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

	if (trace_sort)
		pg_rusage_init(&state->ru_start);

	state->base.sortopt = sortopt;
	state->base.tuples = true;
	state->abbrevNext = 10;

	/*
	 * workMem is forced to be at least 64KB, the current minimum valid value
	 * for the work_mem GUC.  This is a defense against parallel sort callers
	 * that divide out memory among many workers in a way that leaves each
	 * with very little memory.
	 */
	state->allowedMem = Max(workMem, 64) * (int64) 1024;
	state->base.sortcontext = sortcontext;
	state->base.maincontext = maincontext;

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

	oldcontext = MemoryContextSwitchTo(state->base.maincontext);

	/*
	 * Caller tuple (e.g. IndexTuple) memory context.
	 *
	 * A dedicated child context used exclusively for caller passed tuples
	 * eases memory management.  Resetting at key points reduces
	 * fragmentation. Note that the memtuples array of SortTuples is allocated
	 * in the parent context, not this context, because there is no need to
	 * free memtuples early.  For bounded sorts, tuples may be pfreed in any
	 * order, so we use a regular aset.c context so that it can make use of
	 * free'd memory.  When the sort is not bounded, we make use of a bump.c
	 * context as this keeps allocations more compact with less wastage.
	 * Allocations are also slightly more CPU efficient.
	 */
	if (TupleSortUseBumpTupleCxt(state->base.sortopt))
		state->base.tuplecontext = BumpContextCreate(state->base.sortcontext,
													 "Caller tuples",
													 ALLOCSET_DEFAULT_SIZES);
	else
		state->base.tuplecontext = AllocSetContextCreate(state->base.sortcontext,
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
	 * Tape variables (inputTapes, outputTapes, etc.) will be initialized by
	 * inittapes(), if needed.
	 */

	state->result_tape = NULL;	/* flag that result tape has not been formed */

	MemoryContextSwitchTo(oldcontext);
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
	/* Assert we allow bounded sorts */
	Assert(state->base.sortopt & TUPLESORT_ALLOWBOUNDED);
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
	state->base.sortKeys->abbrev_converter = NULL;
	if (state->base.sortKeys->abbrev_full_comparator)
		state->base.sortKeys->comparator = state->base.sortKeys->abbrev_full_comparator;

	/* Not strictly necessary, but be tidy */
	state->base.sortKeys->abbrev_abort = NULL;
	state->base.sortKeys->abbrev_full_comparator = NULL;
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
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);
	int64		spaceUsed;

	if (state->tapeset)
		spaceUsed = LogicalTapeSetBlocks(state->tapeset);
	else
		spaceUsed = (state->allowedMem - state->availMem + 1023) / 1024;

	/*
	 * Delete temporary "tape" files, if any.
	 *
	 * We don't bother to destroy the individual tapes here. They will go away
	 * with the sortcontext.  (In TSS_FINALMERGE state, we have closed
	 * finished tapes already.)
	 */
	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);

	if (trace_sort)
	{
		if (state->tapeset)
			elog(LOG, "%s of worker %d ended, %lld disk blocks used: %s",
				 SERIAL(state) ? "external sort" : "parallel external sort",
				 state->worker, (long long) spaceUsed, pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "%s of worker %d ended, %lld KB used: %s",
				 SERIAL(state) ? "internal sort" : "unperformed parallel sort",
				 state->worker, (long long) spaceUsed, pg_rusage_show(&state->ru_start));
	}

	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, spaceUsed);

	FREESTATE(state);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Free the per-sort memory context, thereby releasing all working memory.
	 */
	MemoryContextReset(state->base.sortcontext);
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
	MemoryContextDelete(state->base.maincontext);
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
 * Shared code for tuple and datum cases.
 */
void
tuplesort_puttuple_common(Tuplesortstate *state, SortTuple *tuple,
						  bool useAbbrev, Size tuplen)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(!LEADER(state));

	/* account for the memory used for this tuple */
	USEMEM(state, tuplen);
	state->tupleMem += tuplen;

	if (!useAbbrev)
	{
		/*
		 * Leave ordinary Datum representation, or NULL value.  If there is a
		 * converter it won't expect NULL values, and cost model is not
		 * required to account for NULL, so in that case we avoid calling
		 * converter and just set datum1 to zeroed representation (to be
		 * consistent, and to support cheap inequality tests for NULL
		 * abbreviated keys).
		 */
	}
	else if (!consider_abort_common(state))
	{
		/* Store abbreviated key representation */
		tuple->datum1 = state->base.sortKeys->abbrev_converter(tuple->datum1,
															   state->base.sortKeys);
	}
	else
	{
		/*
		 * Set state to be consistent with never trying abbreviation.
		 *
		 * Alter datum1 representation in already-copied tuples, so as to
		 * ensure a consistent representation (current tuple was just
		 * handled).  It does not matter if some dumped tuples are already
		 * sorted on tape, since serialized tuples lack abbreviated keys
		 * (TSS_BUILDRUNS state prevents control reaching here in any case).
		 */
		REMOVEABBREV(state, state->memtuples, state->memtupcount);
	}

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
				if (trace_sort)
					elog(LOG, "switching to bounded heapsort at %d tuples: %s",
						 state->memtupcount,
						 pg_rusage_show(&state->ru_start));
				make_bounded_heap(state);
				MemoryContextSwitchTo(oldcontext);
				return;
			}

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
			{
				MemoryContextSwitchTo(oldcontext);
				return;
			}

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
	MemoryContextSwitchTo(oldcontext);
}

static bool
consider_abort_common(Tuplesortstate *state)
{
	Assert(state->base.sortKeys[0].abbrev_converter != NULL);
	Assert(state->base.sortKeys[0].abbrev_abort != NULL);
	Assert(state->base.sortKeys[0].abbrev_full_comparator != NULL);

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
		if (!state->base.sortKeys->abbrev_abort(state->memtupcount,
												state->base.sortKeys))
			return false;

		/*
		 * Finally, restore authoritative comparator, and indicate that
		 * abbreviation is not in play by setting abbrev_converter to NULL
		 */
		state->base.sortKeys[0].comparator = state->base.sortKeys[0].abbrev_full_comparator;
		state->base.sortKeys[0].abbrev_converter = NULL;
		/* Not strictly necessary, but be tidy */
		state->base.sortKeys[0].abbrev_abort = NULL;
		state->base.sortKeys[0].abbrev_full_comparator = NULL;

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
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	if (trace_sort)
		elog(LOG, "performsort of worker %d starting: %s",
			 state->worker, pg_rusage_show(&state->ru_start));

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
			 * have to transform the heap to a properly-sorted array. Note
			 * that sort_bounded_heap sets the correct state->status.
			 */
			sort_bounded_heap(state);
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
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

	if (trace_sort)
	{
		if (state->status == TSS_FINALMERGE)
			elog(LOG, "performsort of worker %d done (except %d-way final merge): %s",
				 state->worker, state->nInputTapes,
				 pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "performsort of worker %d done: %s",
				 state->worker, pg_rusage_show(&state->ru_start));
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Internal routine to fetch the next tuple in either forward or back
 * direction into *stup.  Returns false if no more tuples.
 * Returned tuple belongs to tuplesort memory context, and must not be freed
 * by caller.  Note that fetched tuple is stored in memory that may be
 * recycled by any future fetch.
 */
bool
tuplesort_gettuple_common(Tuplesortstate *state, bool forward,
						  SortTuple *stup)
{
	unsigned int tuplen;
	size_t		nmoved;

	Assert(!WORKER(state));

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			Assert(forward || state->base.sortopt & TUPLESORT_RANDOMACCESS);
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
			Assert(forward || state->base.sortopt & TUPLESORT_RANDOMACCESS);
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

				if ((tuplen = getlen(state->result_tape, true)) != 0)
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
				nmoved = LogicalTapeBackspace(state->result_tape,
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
				nmoved = LogicalTapeBackspace(state->result_tape,
											  sizeof(unsigned int));
				if (nmoved == 0)
					return false;
				else if (nmoved != sizeof(unsigned int))
					elog(ERROR, "unexpected tape position");
				tuplen = getlen(state->result_tape, false);

				/*
				 * Back up to get ending length word of tuple before it.
				 */
				nmoved = LogicalTapeBackspace(state->result_tape,
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

			tuplen = getlen(state->result_tape, false);

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			nmoved = LogicalTapeBackspace(state->result_tape,
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
				int			srcTapeIndex = state->memtuples[0].srctape;
				LogicalTape *srcTape = state->inputTapes[srcTapeIndex];
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
					state->nInputRuns--;

					/*
					 * Close the tape.  It'd go away at the end of the sort
					 * anyway, but better to release the memory early.
					 */
					LogicalTapeClose(srcTape);
					return true;
				}
				newtup.srctape = srcTapeIndex;
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
			oldcontext = MemoryContextSwitchTo(state->base.sortcontext);
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

	/*----------
	 * In the merge phase, we need buffer space for each input and output tape.
	 * Each pass in the balanced merge algorithm reads from M input tapes, and
	 * writes to N output tapes.  Each tape consumes TAPE_BUFFER_OVERHEAD bytes
	 * of memory.  In addition to that, we want MERGE_BUFFER_SIZE workspace per
	 * input tape.
	 *
	 * totalMem = M * (TAPE_BUFFER_OVERHEAD + MERGE_BUFFER_SIZE) +
	 *            N * TAPE_BUFFER_OVERHEAD
	 *
	 * Except for the last and next-to-last merge passes, where there can be
	 * fewer tapes left to process, M = N.  We choose M so that we have the
	 * desired amount of memory available for the input buffers
	 * (TAPE_BUFFER_OVERHEAD + MERGE_BUFFER_SIZE), given the total memory
	 * available for the tape buffers (allowedMem).
	 *
	 * Note: you might be thinking we need to account for the memtuples[]
	 * array in this calculation, but we effectively treat that as part of the
	 * MERGE_BUFFER_SIZE workspace.
	 *----------
	 */
	mOrder = allowedMem /
		(2 * TAPE_BUFFER_OVERHEAD + MERGE_BUFFER_SIZE);

	/*
	 * Even in minimum memory, use at least a MINORDER merge.  On the other
	 * hand, even when we have lots of memory, do not use more than a MAXORDER
	 * merge.  Tapes are pretty cheap, but they're not entirely free.  Each
	 * additional tape reduces the amount of memory available to build runs,
	 * which in turn can cause the same sort to need more runs, which makes
	 * merging slower even if it can still be done in a single pass.  Also,
	 * high order merges are quite slow due to CPU cache effects; it can be
	 * faster to pay the I/O cost of a multi-pass merge than to perform a
	 * single merge pass across many hundreds of tapes.
	 */
	mOrder = Max(mOrder, MINORDER);
	mOrder = Min(mOrder, MAXORDER);

	return mOrder;
}

/*
 * Helper function to calculate how much memory to allocate for the read buffer
 * of each input tape in a merge pass.
 *
 * 'avail_mem' is the amount of memory available for the buffers of all the
 *		tapes, both input and output.
 * 'nInputTapes' and 'nInputRuns' are the number of input tapes and runs.
 * 'maxOutputTapes' is the max. number of output tapes we should produce.
 */
static int64
merge_read_buffer_size(int64 avail_mem, int nInputTapes, int nInputRuns,
					   int maxOutputTapes)
{
	int			nOutputRuns;
	int			nOutputTapes;

	/*
	 * How many output tapes will we produce in this pass?
	 *
	 * This is nInputRuns / nInputTapes, rounded up.
	 */
	nOutputRuns = (nInputRuns + nInputTapes - 1) / nInputTapes;

	nOutputTapes = Min(nOutputRuns, maxOutputTapes);

	/*
	 * Each output tape consumes TAPE_BUFFER_OVERHEAD bytes of memory.  All
	 * remaining memory is divided evenly between the input tapes.
	 *
	 * This also follows from the formula in tuplesort_merge_order, but here
	 * we derive the input buffer size from the amount of memory available,
	 * and M and N.
	 */
	return Max((avail_mem - TAPE_BUFFER_OVERHEAD * nOutputTapes) / nInputTapes, 0);
}

/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we won't sort in memory.
 */
static void
inittapes(Tuplesortstate *state, bool mergeruns)
{
	Assert(!LEADER(state));

	if (mergeruns)
	{
		/* Compute number of input tapes to use when merging */
		state->maxTapes = tuplesort_merge_order(state->allowedMem);
	}
	else
	{
		/* Workers can sometimes produce single run, output without merge */
		Assert(WORKER(state));
		state->maxTapes = MINORDER;
	}

	if (trace_sort)
		elog(LOG, "worker %d switching to external sort with %d tapes: %s",
			 state->worker, state->maxTapes, pg_rusage_show(&state->ru_start));

	/* Create the tape set */
	inittapestate(state, state->maxTapes);
	state->tapeset =
		LogicalTapeSetCreate(false,
							 state->shared ? &state->shared->fileset : NULL,
							 state->worker);

	state->currentRun = 0;

	/*
	 * Initialize logical tape arrays.
	 */
	state->inputTapes = NULL;
	state->nInputTapes = 0;
	state->nInputRuns = 0;

	state->outputTapes = palloc0(state->maxTapes * sizeof(LogicalTape *));
	state->nOutputTapes = 0;
	state->nOutputRuns = 0;

	state->status = TSS_BUILDRUNS;

	selectnewtape(state);
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
}

/*
 * selectnewtape -- select next tape to output to.
 *
 * This is called after finishing a run when we know another run
 * must be started.  This is used both when building the initial
 * runs, and during merge passes.
 */
static void
selectnewtape(Tuplesortstate *state)
{
	/*
	 * At the beginning of each merge pass, nOutputTapes and nOutputRuns are
	 * both zero.  On each call, we create a new output tape to hold the next
	 * run, until maxTapes is reached.  After that, we assign new runs to the
	 * existing tapes in a round robin fashion.
	 */
	if (state->nOutputTapes < state->maxTapes)
	{
		/* Create a new tape to hold the next run */
		Assert(state->outputTapes[state->nOutputRuns] == NULL);
		Assert(state->nOutputRuns == state->nOutputTapes);
		state->destTape = LogicalTapeCreate(state->tapeset);
		state->outputTapes[state->nOutputTapes] = state->destTape;
		state->nOutputTapes++;
		state->nOutputRuns++;
	}
	else
	{
		/*
		 * We have reached the max number of tapes.  Append to an existing
		 * tape.
		 */
		state->destTape = state->outputTapes[state->nOutputRuns % state->nOutputTapes];
		state->nOutputRuns++;
	}
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
 * This implements the Balanced k-Way Merge Algorithm.  All input data has
 * already been written to initial runs on tape (see dumptuples).
 */
static void
mergeruns(Tuplesortstate *state)
{
	int			tapenum;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0);

	if (state->base.sortKeys != NULL && state->base.sortKeys->abbrev_converter != NULL)
	{
		/*
		 * If there are multiple runs to be merged, when we go to read back
		 * tuples from disk, abbreviated keys will not have been stored, and
		 * we don't care to regenerate them.  Disable abbreviation from this
		 * point on.
		 */
		state->base.sortKeys->abbrev_converter = NULL;
		state->base.sortKeys->comparator = state->base.sortKeys->abbrev_full_comparator;

		/* Not strictly necessary, but be tidy */
		state->base.sortKeys->abbrev_abort = NULL;
		state->base.sortKeys->abbrev_full_comparator = NULL;
	}

	/*
	 * Reset tuple memory.  We've freed all the tuples that we previously
	 * allocated.  We will use the slab allocator from now on.
	 */
	MemoryContextResetOnly(state->base.tuplecontext);

	/*
	 * We no longer need a large memtuples array.  (We will allocate a smaller
	 * one for the heap later.)
	 */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	pfree(state->memtuples);
	state->memtuples = NULL;

	/*
	 * Initialize the slab allocator.  We need one slab slot per input tape,
	 * for the tuples in the heap, plus one to hold the tuple last returned
	 * from tuplesort_gettuple.  (If we're sorting pass-by-val Datums,
	 * however, we don't need to do allocate anything.)
	 *
	 * In a multi-pass merge, we could shrink this allocation for the last
	 * merge pass, if it has fewer tapes than previous passes, but we don't
	 * bother.
	 *
	 * From this point on, we no longer use the USEMEM()/LACKMEM() mechanism
	 * to track memory usage of individual tuples.
	 */
	if (state->base.tuples)
		init_slab_allocator(state, state->nOutputTapes + 1);
	else
		init_slab_allocator(state, 0);

	/*
	 * Allocate a new 'memtuples' array, for the heap.  It will hold one tuple
	 * from each input tape.
	 *
	 * We could shrink this, too, between passes in a multi-pass merge, but we
	 * don't bother.  (The initial input tapes are still in outputTapes.  The
	 * number of input tapes will not increase between passes.)
	 */
	state->memtupsize = state->nOutputTapes;
	state->memtuples = (SortTuple *) MemoryContextAlloc(state->base.maincontext,
														state->nOutputTapes * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	/*
	 * Use all the remaining memory we have available for tape buffers among
	 * all the input tapes.  At the beginning of each merge pass, we will
	 * divide this memory between the input and output tapes in the pass.
	 */
	state->tape_buffer_mem = state->availMem;
	USEMEM(state, state->tape_buffer_mem);
	if (trace_sort)
		elog(LOG, "worker %d using %zu KB of memory for tape buffers",
			 state->worker, state->tape_buffer_mem / 1024);

	for (;;)
	{
		/*
		 * On the first iteration, or if we have read all the runs from the
		 * input tapes in a multi-pass merge, it's time to start a new pass.
		 * Rewind all the output tapes, and make them inputs for the next
		 * pass.
		 */
		if (state->nInputRuns == 0)
		{
			int64		input_buffer_size;

			/* Close the old, emptied, input tapes */
			if (state->nInputTapes > 0)
			{
				for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
					LogicalTapeClose(state->inputTapes[tapenum]);
				pfree(state->inputTapes);
			}

			/* Previous pass's outputs become next pass's inputs. */
			state->inputTapes = state->outputTapes;
			state->nInputTapes = state->nOutputTapes;
			state->nInputRuns = state->nOutputRuns;

			/*
			 * Reset output tape variables.  The actual LogicalTapes will be
			 * created as needed, here we only allocate the array to hold
			 * them.
			 */
			state->outputTapes = palloc0(state->nInputTapes * sizeof(LogicalTape *));
			state->nOutputTapes = 0;
			state->nOutputRuns = 0;

			/*
			 * Redistribute the memory allocated for tape buffers, among the
			 * new input and output tapes.
			 */
			input_buffer_size = merge_read_buffer_size(state->tape_buffer_mem,
													   state->nInputTapes,
													   state->nInputRuns,
													   state->maxTapes);

			if (trace_sort)
				elog(LOG, "starting merge pass of %d input runs on %d tapes, " INT64_FORMAT " KB of memory for each input tape: %s",
					 state->nInputRuns, state->nInputTapes, input_buffer_size / 1024,
					 pg_rusage_show(&state->ru_start));

			/* Prepare the new input tapes for merge pass. */
			for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
				LogicalTapeRewindForRead(state->inputTapes[tapenum], input_buffer_size);

			/*
			 * If there's just one run left on each input tape, then only one
			 * merge pass remains.  If we don't have to produce a materialized
			 * sorted tape, we can stop at this point and do the final merge
			 * on-the-fly.
			 */
			if ((state->base.sortopt & TUPLESORT_RANDOMACCESS) == 0
				&& state->nInputRuns <= state->nInputTapes
				&& !WORKER(state))
			{
				/* Tell logtape.c we won't be writing anymore */
				LogicalTapeSetForgetFreeSpace(state->tapeset);
				/* Initialize for the final merge pass */
				beginmerge(state);
				state->status = TSS_FINALMERGE;
				return;
			}
		}

		/* Select an output tape */
		selectnewtape(state);

		/* Merge one run from each input tape. */
		mergeonerun(state);

		/*
		 * If the input tapes are empty, and we output only one output run,
		 * we're done.  The current output tape contains the final result.
		 */
		if (state->nInputRuns == 0 && state->nOutputRuns <= 1)
			break;
	}

	/*
	 * Done.  The result is on a single run on a single tape.
	 */
	state->result_tape = state->outputTapes[0];
	if (!WORKER(state))
		LogicalTapeFreeze(state->result_tape, NULL);
	else
		worker_freeze_result_tape(state);
	state->status = TSS_SORTEDONTAPE;

	/* Close all the now-empty input tapes, to release their read buffers. */
	for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
		LogicalTapeClose(state->inputTapes[tapenum]);
}

/*
 * Merge one run from each input tape.
 */
static void
mergeonerun(Tuplesortstate *state)
{
	int			srcTapeIndex;
	LogicalTape *srcTape;

	/*
	 * Start the merge by loading one tuple from each active source tape into
	 * the heap.
	 */
	beginmerge(state);

	Assert(state->slabAllocatorUsed);

	/*
	 * Execute merge by repeatedly extracting lowest tuple in heap, writing it
	 * out, and replacing it with next tuple from same tape (if there is
	 * another one).
	 */
	while (state->memtupcount > 0)
	{
		SortTuple	stup;

		/* write the tuple to destTape */
		srcTapeIndex = state->memtuples[0].srctape;
		srcTape = state->inputTapes[srcTapeIndex];
		WRITETUP(state, state->destTape, &state->memtuples[0]);

		/* recycle the slot of the tuple we just wrote out, for the next read */
		if (state->memtuples[0].tuple)
			RELEASE_SLAB_SLOT(state, state->memtuples[0].tuple);

		/*
		 * pull next tuple from the tape, and replace the written-out tuple in
		 * the heap with it.
		 */
		if (mergereadnext(state, srcTape, &stup))
		{
			stup.srctape = srcTapeIndex;
			tuplesort_heap_replace_top(state, &stup);
		}
		else
		{
			tuplesort_heap_delete_top(state);
			state->nInputRuns--;
		}
	}

	/*
	 * When the heap empties, we're done.  Write an end-of-run marker on the
	 * output tape.
	 */
	markrunend(state->destTape);
}

/*
 * beginmerge - initialize for a merge pass
 *
 * Fill the merge heap with the first tuple from each input tape.
 */
static void
beginmerge(Tuplesortstate *state)
{
	int			activeTapes;
	int			srcTapeIndex;

	/* Heap should be empty here */
	Assert(state->memtupcount == 0);

	activeTapes = Min(state->nInputTapes, state->nInputRuns);

	for (srcTapeIndex = 0; srcTapeIndex < activeTapes; srcTapeIndex++)
	{
		SortTuple	tup;

		if (mergereadnext(state, state->inputTapes[srcTapeIndex], &tup))
		{
			tup.srctape = srcTapeIndex;
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
mergereadnext(Tuplesortstate *state, LogicalTape *srcTape, SortTuple *stup)
{
	unsigned int tuplen;

	/* read next tuple, if any */
	if ((tuplen = getlen(srcTape, true)) == 0)
		return false;
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
	 * exhausted.  In general, short final runs are quite possible, but avoid
	 * creating a completely empty run.  In a worker, though, we must produce
	 * at least one tape, even if it's empty.
	 */
	if (state->memtupcount == 0 && state->currentRun > 0)
		return;

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

	if (state->currentRun > 0)
		selectnewtape(state);

	state->currentRun++;

	if (trace_sort)
		elog(LOG, "worker %d starting quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));

	/*
	 * Sort all tuples accumulated within the allowed amount of memory for
	 * this run using quicksort
	 */
	tuplesort_sort_memtuples(state);

	if (trace_sort)
		elog(LOG, "worker %d finished quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));

	memtupwrite = state->memtupcount;
	for (i = 0; i < memtupwrite; i++)
	{
		SortTuple  *stup = &state->memtuples[i];

		WRITETUP(state, state->destTape, stup);
	}

	state->memtupcount = 0;

	/*
	 * Reset tuple memory.  We've freed all of the tuples that we previously
	 * allocated.  It's important to avoid fragmentation when there is a stark
	 * change in the sizes of incoming tuples.  In bounded sorts,
	 * fragmentation due to AllocSetFree's bucketing by size class might be
	 * particularly bad if this step wasn't taken.
	 */
	MemoryContextReset(state->base.tuplecontext);

	/*
	 * Now update the memory accounting to subtract the memory used by the
	 * tuple.
	 */
	FREEMEM(state, state->tupleMem);
	state->tupleMem = 0;

	markrunend(state->destTape);

	if (trace_sort)
		elog(LOG, "worker %d finished writing run %d to tape %d: %s",
			 state->worker, state->currentRun, (state->currentRun - 1) % state->nOutputTapes + 1,
			 pg_rusage_show(&state->ru_start));
}

/*
 * tuplesort_rescan		- rewind and replay the scan
 */
void
tuplesort_rescan(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeRewindForRead(state->result_tape, 0);
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
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->markpos_offset = state->current;
			state->markpos_eof = state->eof_reached;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeTell(state->result_tape,
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
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeSeek(state->result_tape,
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
		/*
		 * Do we have the leading column's value or abbreviation in datum1,
		 * and is there a specialization for its comparator?
		 */
		if (state->base.haveDatum1 && state->base.sortKeys)
		{
			if (state->base.sortKeys[0].comparator == ssup_datum_unsigned_cmp)
			{
				qsort_tuple_unsigned(state->memtuples,
									 state->memtupcount,
									 state);
				return;
			}
#if SIZEOF_DATUM >= 8
			else if (state->base.sortKeys[0].comparator == ssup_datum_signed_cmp)
			{
				qsort_tuple_signed(state->memtuples,
								   state->memtupcount,
								   state);
				return;
			}
#endif
			else if (state->base.sortKeys[0].comparator == ssup_datum_int32_cmp)
			{
				qsort_tuple_int32(state->memtuples,
								  state->memtupcount,
								  state);
				return;
			}
		}

		/* Can we use the single-key sort function? */
		if (state->base.onlyKey != NULL)
		{
			qsort_ssup(state->memtuples, state->memtupcount,
					   state->base.onlyKey);
		}
		else
		{
			qsort_tuple(state->memtuples,
						state->memtupcount,
						state->base.comparetup,
						state);
		}
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
	SortSupport sortKey = state->base.sortKeys;
	int			nkey;

	for (nkey = 0; nkey < state->base.nKeys; nkey++, sortKey++)
	{
		sortKey->ssup_reverse = !sortKey->ssup_reverse;
		sortKey->ssup_nulls_first = !sortKey->ssup_nulls_first;
	}
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(LogicalTape *tape, bool eofOK)
{
	unsigned int len;

	if (LogicalTapeRead(tape,
						&len, sizeof(len)) != sizeof(len))
		elog(ERROR, "unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "unexpected end of data");
	return len;
}

static void
markrunend(LogicalTape *tape)
{
	unsigned int len = 0;

	LogicalTapeWrite(tape, &len, sizeof(len));
}

/*
 * Get memory for tuple from within READTUP() routine.
 *
 * We use next free slot from the slab allocator, or palloc() if the tuple
 * is too large for that.
 */
void *
tuplesort_readtup_alloc(Tuplesortstate *state, Size tuplen)
{
	SlabSlot   *buf;

	/*
	 * We pre-allocate enough slots in the slab arena that we should never run
	 * out.
	 */
	Assert(state->slabFreeHead);

	if (tuplen > SLAB_SLOT_SIZE || !state->slabFreeHead)
		return MemoryContextAlloc(state->base.sortcontext, tuplen);
	else
	{
		buf = state->slabFreeHead;
		/* Reuse this slot */
		state->slabFreeHead = buf->nextfree;

		return buf;
	}
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
	Assert(state->result_tape != NULL);
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
	LogicalTapeFreeze(state->result_tape, &output);

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
	Assert(state->result_tape == NULL);
	Assert(state->nOutputRuns == 1);

	state->result_tape = state->destTape;
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
	 */
	inittapestate(state, nParticipants);
	state->tapeset = LogicalTapeSetCreate(false, &shared->fileset, -1);

	/*
	 * Set currentRun to reflect the number of runs we will merge (it's not
	 * used for anything, this is just pro forma)
	 */
	state->currentRun = nParticipants;

	/*
	 * Initialize the state to look the same as after building the initial
	 * runs.
	 *
	 * There will always be exactly 1 run per worker, and exactly one input
	 * tape per run, because workers always output exactly 1 run, even when
	 * there were no input tuples for workers to sort.
	 */
	state->inputTapes = NULL;
	state->nInputTapes = 0;
	state->nInputRuns = 0;

	state->outputTapes = palloc0(nParticipants * sizeof(LogicalTape *));
	state->nOutputTapes = nParticipants;
	state->nOutputRuns = nParticipants;

	for (j = 0; j < nParticipants; j++)
	{
		state->outputTapes[j] = LogicalTapeImport(state->tapeset, j, &shared->tapes[j]);
	}

	state->status = TSS_BUILDRUNS;
}

/*
 * Convenience routine to free a tuple previously loaded into sort memory
 */
static void
free_sort_tuple(Tuplesortstate *state, SortTuple *stup)
{
	if (stup->tuple)
	{
		FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
		pfree(stup->tuple);
		stup->tuple = NULL;
	}
}

int
ssup_datum_unsigned_cmp(Datum x, Datum y, SortSupport ssup)
{
	if (x < y)
		return -1;
	else if (x > y)
		return 1;
	else
		return 0;
}

#if SIZEOF_DATUM >= 8
int
ssup_datum_signed_cmp(Datum x, Datum y, SortSupport ssup)
{
	int64		xx = DatumGetInt64(x);
	int64		yy = DatumGetInt64(y);

	if (xx < yy)
		return -1;
	else if (xx > yy)
		return 1;
	else
		return 0;
}
#endif

int
ssup_datum_int32_cmp(Datum x, Datum y, SortSupport ssup)
{
	int32		xx = DatumGetInt32(x);
	int32		yy = DatumGetInt32(y);

	if (xx < yy)
		return -1;
	else if (xx > yy)
		return 1;
	else
		return 0;
}
