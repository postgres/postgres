/*-------------------------------------------------------------------------
 *
 * tuplestore.c
 *	  Generalized routines for temporary tuple storage.
 *
 * This module handles temporary storage of tuples for purposes such
 * as Materialize nodes, hashjoin batch files, etc.  It is essentially
 * a dumbed-down version of tuplesort.c; it does no sorting of tuples
 * but can only store and regurgitate a sequence of tuples.  However,
 * because no sort is required, it is allowed to start reading the sequence
 * before it has all been written.	This is particularly useful for cursors,
 * because it allows random access within the already-scanned portion of
 * a query without having to process the underlying scan to completion.
 * A temporary file is used to handle the data if it exceeds the
 * space limit specified by the caller.
 *
 * The (approximate) amount of memory allowed to the tuplestore is specified
 * in kilobytes by the caller.	We absorb tuples and simply store them in an
 * in-memory array as long as we haven't exceeded maxKBytes.  If we do exceed
 * maxKBytes, we dump all the tuples into a temp file and then read from that
 * when needed.
 *
 * When the caller requests backward-scan capability, we write the temp file
 * in a format that allows either forward or backward scan.  Otherwise, only
 * forward scan is allowed.  Rewind and markpos/restorepos are normally allowed
 * but can be turned off via tuplestore_set_eflags; turning off both backward
 * scan and rewind enables truncation of the tuplestore at the mark point
 * (if any) for minimal memory usage.
 *
 * Because we allow reading before writing is complete, there are two
 * interesting positions in the temp file: the current read position and
 * the current write position.	At any given instant, the temp file's seek
 * position corresponds to one of these, and the other one is remembered in
 * the Tuplestore's state.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/sort/tuplestore.c,v 1.36 2008/01/01 19:45:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "storage/buffile.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"


/*
 * Possible states of a Tuplestore object.	These denote the states that
 * persist between calls of Tuplestore routines.
 */
typedef enum
{
	TSS_INMEM,					/* Tuples still fit in memory */
	TSS_WRITEFILE,				/* Writing to temp file */
	TSS_READFILE				/* Reading from temp file */
} TupStoreStatus;

/*
 * Private state of a Tuplestore operation.
 */
struct Tuplestorestate
{
	TupStoreStatus status;		/* enumerated value as shown above */
	int			eflags;			/* capability flags */
	bool		interXact;		/* keep open through transactions? */
	long		availMem;		/* remaining memory available, in bytes */
	BufFile    *myfile;			/* underlying file, or NULL if none */

	/*
	 * These function pointers decouple the routines that must know what kind
	 * of tuple we are handling from the routines that don't need to know it.
	 * They are set up by the tuplestore_begin_xxx routines.
	 *
	 * (Although tuplestore.c currently only supports heap tuples, I've copied
	 * this part of tuplesort.c so that extension to other kinds of objects
	 * will be easy if it's ever needed.)
	 *
	 * Function to copy a supplied input tuple into palloc'd space. (NB: we
	 * assume that a single pfree() is enough to release the tuple later, so
	 * the representation must be "flat" in one palloc chunk.) state->availMem
	 * must be decreased by the amount of space used.
	 */
	void	   *(*copytup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to write a stored tuple onto tape.	The representation of the
	 * tuple on tape need not be the same as it is in memory; requirements on
	 * the tape representation are given below.  After writing the tuple,
	 * pfree() it, and increase state->availMem by the amount of memory space
	 * thereby released.
	 */
	void		(*writetup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len' is
	 * the already-read length of the stored tuple.  Create and return a
	 * palloc'd copy, and decrease state->availMem by the amount of memory
	 * space consumed.
	 */
	void	   *(*readtup) (Tuplestorestate *state, unsigned int len);

	/*
	 * This array holds pointers to tuples in memory if we are in state INMEM.
	 * In states WRITEFILE and READFILE it's not used.
	 */
	void	  **memtuples;		/* array of pointers to palloc'd tuples */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */

	/*
	 * These variables are used to keep track of the current position.
	 *
	 * In state WRITEFILE, the current file seek position is the write point,
	 * and the read position is remembered in readpos_xxx; in state READFILE,
	 * the current file seek position is the read point, and the write
	 * position is remembered in writepos_xxx.	(The write position is the
	 * same as EOF, but since BufFileSeek doesn't currently implement
	 * SEEK_END, we have to remember it explicitly.)
	 *
	 * Special case: if we are in WRITEFILE state and eof_reached is true,
	 * then the read position is implicitly equal to the write position (and
	 * hence to the file seek position); this way we need not update the
	 * readpos_xxx variables on each write.
	 */
	bool		eof_reached;	/* read reached EOF (always valid) */
	int			current;		/* next array index (valid if INMEM) */
	int			readpos_file;	/* file# (valid if WRITEFILE and not eof) */
	long		readpos_offset; /* offset (valid if WRITEFILE and not eof) */
	int			writepos_file;	/* file# (valid if READFILE) */
	long		writepos_offset;	/* offset (valid if READFILE) */

	/* markpos_xxx holds marked position for mark and restore */
	int			markpos_current;	/* saved "current" */
	int			markpos_file;	/* saved "readpos_file" */
	long		markpos_offset; /* saved "readpos_offset" */
};

#define COPYTUP(state,tup)	((*(state)->copytup) (state, tup))
#define WRITETUP(state,tup) ((*(state)->writetup) (state, tup))
#define READTUP(state,len)	((*(state)->readtup) (state, len))
#define LACKMEM(state)		((state)->availMem < 0)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))

/*--------------------
 *
 * NOTES about on-tape representation of tuples:
 *
 * We require the first "unsigned int" of a stored tuple to be the total size
 * on-tape of the tuple, including itself (so it is never zero).
 * The remainder of the stored tuple
 * may or may not match the in-memory representation of the tuple ---
 * any conversion needed is the job of the writetup and readtup routines.
 *
 * If state->eflags & EXEC_FLAG_BACKWARD, then the stored representation of
 * the tuple must be followed by another "unsigned int" that is a copy of the
 * length --- so the total tape space used is actually sizeof(unsigned int)
 * more than the stored length value.  This allows read-backwards.	When
 * EXEC_FLAG_BACKWARD is not set, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplestorestate record, if needed. They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the maxKBytes limit,
 * plus the space used by the variable-size array memtuples.
 * Fixed-size space (primarily the BufFile I/O buffer) is not counted.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.
 *
 *--------------------
 */


static Tuplestorestate *tuplestore_begin_common(int eflags,
						bool interXact,
						int maxKBytes);
static void tuplestore_puttuple_common(Tuplestorestate *state, void *tuple);
static void dumptuples(Tuplestorestate *state);
static void tuplestore_trim(Tuplestorestate *state, int ntuples);
static unsigned int getlen(Tuplestorestate *state, bool eofOK);
static void *copytup_heap(Tuplestorestate *state, void *tup);
static void writetup_heap(Tuplestorestate *state, void *tup);
static void *readtup_heap(Tuplestorestate *state, unsigned int len);


/*
 *		tuplestore_begin_xxx
 *
 * Initialize for a tuple store operation.
 */
static Tuplestorestate *
tuplestore_begin_common(int eflags, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;

	state = (Tuplestorestate *) palloc0(sizeof(Tuplestorestate));

	state->status = TSS_INMEM;
	state->eflags = eflags;
	state->interXact = interXact;
	state->availMem = maxKBytes * 1024L;
	state->myfile = NULL;

	state->memtupcount = 0;
	state->memtupsize = 1024;	/* initial guess */
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	state->eof_reached = false;
	state->current = 0;

	return state;
}

/*
 * tuplestore_begin_heap
 *
 * Create a new tuplestore; other types of tuple stores (other than
 * "heap" tuple stores, for heap tuples) are possible, but not presently
 * implemented.
 *
 * randomAccess: if true, both forward and backward accesses to the
 * tuple store are allowed.
 *
 * interXact: if true, the files used for on-disk storage persist beyond the
 * end of the current transaction.	NOTE: It's the caller's responsibility to
 * create such a tuplestore in a memory context that will also survive
 * transaction boundaries, and to ensure the tuplestore is closed when it's
 * no longer wanted.
 *
 * maxKBytes: how much data to store in memory (any data beyond this
 * amount is paged to disk).  When in doubt, use work_mem.
 */
Tuplestorestate *
tuplestore_begin_heap(bool randomAccess, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;
	int			eflags;

	/*
	 * This interpretation of the meaning of randomAccess is compatible with
	 * the pre-8.3 behavior of tuplestores.
	 */
	eflags = randomAccess ?
		(EXEC_FLAG_BACKWARD | EXEC_FLAG_REWIND | EXEC_FLAG_MARK) :
		(EXEC_FLAG_REWIND | EXEC_FLAG_MARK);

	state = tuplestore_begin_common(eflags, interXact, maxKBytes);

	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	return state;
}

/*
 * tuplestore_set_eflags
 *
 * Set capability flags at a finer grain than is allowed by
 * tuplestore_begin_xxx.  This must be called before inserting any data
 * into the tuplestore.
 *
 * eflags is a bitmask following the meanings used for executor node
 * startup flags (see executor.h).	tuplestore pays attention to these bits:
 *		EXEC_FLAG_REWIND		need rewind to start
 *		EXEC_FLAG_BACKWARD		need backward fetch
 *		EXEC_FLAG_MARK			need mark/restore
 * If tuplestore_set_eflags is not called, REWIND and MARK are allowed,
 * and BACKWARD is set per "randomAccess" in the tuplestore_begin_xxx call.
 */
void
tuplestore_set_eflags(Tuplestorestate *state, int eflags)
{
	Assert(state->status == TSS_INMEM);
	Assert(state->memtupcount == 0);

	state->eflags = eflags;
}

/*
 * tuplestore_end
 *
 *	Release resources and clean up.
 */
void
tuplestore_end(Tuplestorestate *state)
{
	int			i;

	if (state->myfile)
		BufFileClose(state->myfile);
	if (state->memtuples)
	{
		for (i = 0; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	pfree(state);
}

/*
 * tuplestore_ateof
 *
 * Returns the current eof_reached state.
 */
bool
tuplestore_ateof(Tuplestorestate *state)
{
	return state->eof_reached;
}

/*
 * Accept one tuple and append it to the tuplestore.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 *
 * If the read status is currently "AT EOF" then it remains so (the read
 * pointer advances along with the write pointer); otherwise the read
 * pointer is unchanged.  This is for the convenience of nodeMaterial.c.
 *
 * tuplestore_puttupleslot() is a convenience routine to collect data from
 * a TupleTableSlot without an extra copy operation.
 */
void
tuplestore_puttupleslot(Tuplestorestate *state,
						TupleTableSlot *slot)
{
	MinimalTuple tuple;

	/*
	 * Form a MinimalTuple in working memory
	 */
	tuple = ExecCopySlotMinimalTuple(slot);
	USEMEM(state, GetMemoryChunkSpace(tuple));

	tuplestore_puttuple_common(state, (void *) tuple);
}

/*
 * "Standard" case to copy from a HeapTuple.  This is actually now somewhat
 * deprecated, but not worth getting rid of in view of the number of callers.
 * (Consider adding something that takes a tupdesc+values/nulls arrays so
 * that we can use heap_form_minimal_tuple() and avoid a copy step.)
 */
void
tuplestore_puttuple(Tuplestorestate *state, HeapTuple tuple)
{
	/*
	 * Copy the tuple.	(Must do this even in WRITEFILE case.)
	 */
	tuple = COPYTUP(state, tuple);

	tuplestore_puttuple_common(state, (void *) tuple);
}

static void
tuplestore_puttuple_common(Tuplestorestate *state, void *tuple)
{
	switch (state->status)
	{
		case TSS_INMEM:

			/*
			 * Grow the array as needed.  Note that we try to grow the array
			 * when there is still one free slot remaining --- if we fail,
			 * there'll still be room to store the incoming tuple, and then
			 * we'll switch to tape-based operation.
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				/*
				 * See grow_memtuples() in tuplesort.c for the rationale
				 * behind these two tests.
				 */
				if (state->availMem > (long) (state->memtupsize * sizeof(void *)) &&
					(Size) (state->memtupsize * 2) < MaxAllocSize / sizeof(void *))
				{
					FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
					state->memtupsize *= 2;
					state->memtuples = (void **)
						repalloc(state->memtuples,
								 state->memtupsize * sizeof(void *));
					USEMEM(state, GetMemoryChunkSpace(state->memtuples));
				}
			}

			/* Stash the tuple in the in-memory array */
			state->memtuples[state->memtupcount++] = tuple;

			/* If eof_reached, keep read position in sync */
			if (state->eof_reached)
				state->current = state->memtupcount;

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.  Make sure that
			 * the temp file(s) are created in suitable temp tablespaces.
			 */
			PrepareTempTablespaces();
			state->myfile = BufFileCreateTemp(state->interXact);
			state->status = TSS_WRITEFILE;
			dumptuples(state);
			break;
		case TSS_WRITEFILE:
			WRITETUP(state, tuple);
			break;
		case TSS_READFILE:

			/*
			 * Switch from reading to writing.
			 */
			if (!state->eof_reached)
				BufFileTell(state->myfile,
							&state->readpos_file, &state->readpos_offset);
			if (BufFileSeek(state->myfile,
							state->writepos_file, state->writepos_offset,
							SEEK_SET) != 0)
				elog(ERROR, "seek to EOF failed");
			state->status = TSS_WRITEFILE;
			WRITETUP(state, tuple);
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 *
 * Backward scan is only allowed if randomAccess was set true or
 * EXEC_FLAG_BACKWARD was specified to tuplestore_set_eflags().
 */
static void *
tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free)
{
	unsigned int tuplen;
	void	   *tup;

	Assert(forward || (state->eflags & EXEC_FLAG_BACKWARD));

	switch (state->status)
	{
		case TSS_INMEM:
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

		case TSS_WRITEFILE:
			/* Skip state change if we'll just return NULL */
			if (state->eof_reached && forward)
				return NULL;

			/*
			 * Switch from writing to reading.
			 */
			BufFileTell(state->myfile,
						&state->writepos_file, &state->writepos_offset);
			if (!state->eof_reached)
				if (BufFileSeek(state->myfile,
								state->readpos_file, state->readpos_offset,
								SEEK_SET) != 0)
					elog(ERROR, "seek failed");
			state->status = TSS_READFILE;
			/* FALL THRU into READFILE case */

		case TSS_READFILE:
			*should_free = true;
			if (forward)
			{
				if ((tuplen = getlen(state, true)) != 0)
				{
					tup = READTUP(state, tuplen);
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
			 *
			 * Back up to fetch previously-returned tuple's ending length
			 * word. If seek fails, assume we are at start of file.
			 */
			if (BufFileSeek(state->myfile, 0, -(long) sizeof(unsigned int),
							SEEK_CUR) != 0)
				return NULL;
			tuplen = getlen(state, false);

			if (state->eof_reached)
			{
				state->eof_reached = false;
				/* We will return the tuple returned before returning NULL */
			}
			else
			{
				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (BufFileSeek(state->myfile, 0,
								-(long) (tuplen + 2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
				{
					/*
					 * If that fails, presumably the prev tuple is the first
					 * in the file.  Back up so that it becomes next to read
					 * in forward direction (not obviously right, but that is
					 * what in-memory case does).
					 */
					if (BufFileSeek(state->myfile, 0,
									-(long) (tuplen + sizeof(unsigned int)),
									SEEK_CUR) != 0)
						elog(ERROR, "bogus tuple length in backward scan");
					return NULL;
				}
				tuplen = getlen(state, false);
			}

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			if (BufFileSeek(state->myfile, 0,
							-(long) tuplen,
							SEEK_CUR) != 0)
				elog(ERROR, "bogus tuple length in backward scan");
			tup = READTUP(state, tuplen);
			return tup;

		default:
			elog(ERROR, "invalid tuplestore state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * tuplestore_gettupleslot - exported function to fetch a MinimalTuple
 *
 * If successful, put tuple in slot and return TRUE; else, clear the slot
 * and return FALSE.
 */
bool
tuplestore_gettupleslot(Tuplestorestate *state, bool forward,
						TupleTableSlot *slot)
{
	MinimalTuple tuple;
	bool		should_free;

	tuple = (MinimalTuple) tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * tuplestore_advance - exported function to adjust position without fetching
 *
 * We could optimize this case to avoid palloc/pfree overhead, but for the
 * moment it doesn't seem worthwhile.
 */
bool
tuplestore_advance(Tuplestorestate *state, bool forward)
{
	void	   *tuple;
	bool		should_free;

	tuple = tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		if (should_free)
			pfree(tuple);
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * dumptuples - remove tuples from memory and write to tape
 *
 * As a side effect, we must set readpos and markpos to the value
 * corresponding to "current"; otherwise, a dump would lose the current read
 * position.
 */
static void
dumptuples(Tuplestorestate *state)
{
	int			i;

	for (i = 0;; i++)
	{
		if (i == state->current)
			BufFileTell(state->myfile,
						&state->readpos_file, &state->readpos_offset);
		if (i == state->markpos_current)
			BufFileTell(state->myfile,
						&state->markpos_file, &state->markpos_offset);
		if (i >= state->memtupcount)
			break;
		WRITETUP(state, state->memtuples[i]);
	}
	state->memtupcount = 0;
}

/*
 * tuplestore_rescan		- rewind and replay the scan
 */
void
tuplestore_rescan(Tuplestorestate *state)
{
	Assert(state->eflags & EXEC_FLAG_REWIND);

	switch (state->status)
	{
		case TSS_INMEM:
			state->eof_reached = false;
			state->current = 0;
			break;
		case TSS_WRITEFILE:
			state->eof_reached = false;
			state->readpos_file = 0;
			state->readpos_offset = 0L;
			break;
		case TSS_READFILE:
			state->eof_reached = false;
			if (BufFileSeek(state->myfile, 0, 0L, SEEK_SET) != 0)
				elog(ERROR, "seek to start failed");
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_markpos	- saves current position in the tuple sequence
 */
void
tuplestore_markpos(Tuplestorestate *state)
{
	Assert(state->eflags & EXEC_FLAG_MARK);

	switch (state->status)
	{
		case TSS_INMEM:
			state->markpos_current = state->current;

			/*
			 * We can truncate the tuplestore if neither backward scan nor
			 * rewind capability are required by the caller.  There will never
			 * be a need to back up past the mark point.
			 *
			 * Note: you might think we could remove all the tuples before
			 * "current", since that one is the next to be returned.  However,
			 * since tuplestore_gettuple returns a direct pointer to our
			 * internal copy of the tuple, it's likely that the caller has
			 * still got the tuple just before "current" referenced in a slot.
			 * Don't free it yet.
			 */
			if (!(state->eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_REWIND)))
				tuplestore_trim(state, 1);
			break;
		case TSS_WRITEFILE:
			if (state->eof_reached)
			{
				/* Need to record the implicit read position */
				BufFileTell(state->myfile,
							&state->markpos_file,
							&state->markpos_offset);
			}
			else
			{
				state->markpos_file = state->readpos_file;
				state->markpos_offset = state->readpos_offset;
			}
			break;
		case TSS_READFILE:
			BufFileTell(state->myfile,
						&state->markpos_file,
						&state->markpos_offset);
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_restorepos - restores current position in tuple sequence to
 *						  last saved position
 */
void
tuplestore_restorepos(Tuplestorestate *state)
{
	Assert(state->eflags & EXEC_FLAG_MARK);

	switch (state->status)
	{
		case TSS_INMEM:
			state->eof_reached = false;
			state->current = state->markpos_current;
			break;
		case TSS_WRITEFILE:
			state->eof_reached = false;
			state->readpos_file = state->markpos_file;
			state->readpos_offset = state->markpos_offset;
			break;
		case TSS_READFILE:
			state->eof_reached = false;
			if (BufFileSeek(state->myfile,
							state->markpos_file,
							state->markpos_offset,
							SEEK_SET) != 0)
				elog(ERROR, "tuplestore_restorepos failed");
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_trim	- remove all but ntuples tuples before current
 */
static void
tuplestore_trim(Tuplestorestate *state, int ntuples)
{
	int			nremove;
	int			i;

	/*
	 * We don't bother trimming temp files since it usually would mean more
	 * work than just letting them sit in kernel buffers until they age out.
	 */
	if (state->status != TSS_INMEM)
		return;

	nremove = state->current - ntuples;
	if (nremove <= 0)
		return;					/* nothing to do */
	Assert(nremove <= state->memtupcount);

	/* Release no-longer-needed tuples */
	for (i = 0; i < nremove; i++)
	{
		FREEMEM(state, GetMemoryChunkSpace(state->memtuples[i]));
		pfree(state->memtuples[i]);
	}

	/*
	 * Slide the array down and readjust pointers.	This may look pretty
	 * stupid, but we expect that there will usually not be very many
	 * tuple-pointers to move, so this isn't that expensive; and it keeps a
	 * lot of other logic simple.
	 *
	 * In fact, in the current usage for merge joins, it's demonstrable that
	 * there will always be exactly one non-removed tuple; so optimize that
	 * case.
	 */
	if (nremove + 1 == state->memtupcount)
		state->memtuples[0] = state->memtuples[nremove];
	else
		memmove(state->memtuples, state->memtuples + nremove,
				(state->memtupcount - nremove) * sizeof(void *));

	state->memtupcount -= nremove;
	state->current -= nremove;
	state->markpos_current -= nremove;
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplestorestate *state, bool eofOK)
{
	unsigned int len;
	size_t		nbytes;

	nbytes = BufFileRead(state->myfile, (void *) &len, sizeof(len));
	if (nbytes == sizeof(len))
		return len;
	if (nbytes != 0)
		elog(ERROR, "unexpected end of tape");
	if (!eofOK)
		elog(ERROR, "unexpected end of data");
	return 0;
}


/*
 * Routines specialized for HeapTuple case
 *
 * The stored form is actually a MinimalTuple, but for largely historical
 * reasons we allow COPYTUP to work from a HeapTuple.
 *
 * Since MinimalTuple already has length in its first word, we don't need
 * to write that separately.
 */

static void *
copytup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple;

	tuple = minimal_tuple_from_heap_tuple((HeapTuple) tup);
	USEMEM(state, GetMemoryChunkSpace(tuple));
	return (void *) tuple;
}

static void
writetup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple = (MinimalTuple) tup;
	unsigned int tuplen = tuple->t_len;

	if (BufFileWrite(state->myfile, (void *) tuple, tuplen) != (size_t) tuplen)
		elog(ERROR, "write failed");
	if (state->eflags & EXEC_FLAG_BACKWARD)		/* need trailing length word? */
		if (BufFileWrite(state->myfile, (void *) &tuplen,
						 sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "write failed");

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(tuple);
}

static void *
readtup_heap(Tuplestorestate *state, unsigned int len)
{
	MinimalTuple tuple = (MinimalTuple) palloc(len);
	unsigned int tuplen;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* read in the tuple proper */
	tuple->t_len = len;
	if (BufFileRead(state->myfile, (void *) ((char *) tuple + sizeof(int)),
					len - sizeof(int)) != (size_t) (len - sizeof(int)))
		elog(ERROR, "unexpected end of data");
	if (state->eflags & EXEC_FLAG_BACKWARD)		/* need trailing length word? */
		if (BufFileRead(state->myfile, (void *) &tuplen,
						sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "unexpected end of data");
	return (void *) tuple;
}
