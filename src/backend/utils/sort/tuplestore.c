/*-------------------------------------------------------------------------
 *
 * tuplestore.c
 *	  Generalized routines for temporary tuple storage.
 *
 * This module handles temporary storage of tuples for purposes such
 * as Materialize nodes, hashjoin batch files, etc.  It is essentially
 * a dumbed-down version of tuplesort.c; it does no sorting of tuples
 * but can only store a sequence of tuples and regurgitate it later.
 * A temporary file is used to handle the data if it exceeds the
 * space limit specified by the caller.
 *
 * The (approximate) amount of memory allowed to the tuplestore is specified
 * in kilobytes by the caller.	We absorb tuples and simply store them in an
 * in-memory array as long as we haven't exceeded maxKBytes.  If we reach the
 * end of the input without exceeding maxKBytes, we just return tuples during
 * the read phase by scanning the tuple array sequentially.  If we do exceed
 * maxKBytes, we dump all the tuples into a temp file and then read from that
 * during the read phase.
 *
 * When the caller requests random access to the data, we write the temp file
 * in a format that allows either forward or backward scan.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/sort/tuplestore.c,v 1.3 2001/03/22 04:00:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "storage/buffile.h"
#include "utils/tuplestore.h"

/*
 * Possible states of a Tuplestore object.	These denote the states that
 * persist between calls of Tuplestore routines.
 */
typedef enum
{
	TSS_INITIAL,				/* Loading tuples; still within memory
								 * limit */
	TSS_WRITEFILE,				/* Loading tuples; writing to temp file */
	TSS_READMEM,				/* Reading tuples; entirely in memory */
	TSS_READFILE				/* Reading tuples from temp file */
} TupStoreStatus;

/*
 * Private state of a Tuplestore operation.
 */
struct Tuplestorestate
{
	TupStoreStatus status;		/* enumerated value as shown above */
	bool		randomAccess;	/* did caller request random access? */
	long		availMem;		/* remaining memory available, in bytes */
	BufFile    *myfile;			/* underlying file, or NULL if none */

	/*
	 * These function pointers decouple the routines that must know what
	 * kind of tuple we are handling from the routines that don't need to
	 * know it. They are set up by the tuplestore_begin_xxx routines.
	 *
	 * (Although tuplestore.c currently only supports heap tuples, I've
	 * copied this part of tuplesort.c so that extension to other kinds of
	 * objects will be easy if it's ever needed.)
	 *
	 * Function to copy a supplied input tuple into palloc'd space. (NB: we
	 * assume that a single pfree() is enough to release the tuple later,
	 * so the representation must be "flat" in one palloc chunk.)
	 * state->availMem must be decreased by the amount of space used.
	 */
	void	   *(*copytup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to write a stored tuple onto tape.	The representation of
	 * the tuple on tape need not be the same as it is in memory;
	 * requirements on the tape representation are given below.  After
	 * writing the tuple, pfree() it, and increase state->availMem by the
	 * amount of memory space thereby released.
	 */
	void		(*writetup) (Tuplestorestate *state, void *tup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len'
	 * is the already-read length of the stored tuple.	Create and return
	 * a palloc'd copy, and decrease state->availMem by the amount of
	 * memory space consumed.
	 */
	void	   *(*readtup) (Tuplestorestate *state, unsigned int len);

	/*
	 * This array holds pointers to tuples in memory if we are in state
	 * INITIAL or READMEM.	In states WRITEFILE and READFILE it's not
	 * used.
	 */
	void	  **memtuples;		/* array of pointers to palloc'd tuples */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */

	/*
	 * These variables are used after completion of storing to keep track
	 * of the next tuple to return.  (In the tape case, the tape's current
	 * read position is also critical state.)
	 */
	int			current;		/* array index (only used if READMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	int			markpos_file;	/* file# (only used if READFILE) */
	long		markpos_offset; /* saved "current", or offset in tape file */
	bool		markpos_eof;	/* saved "eof_reached" */
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
 * stored in the Tuplestorestate record, if needed. They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should elog() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space requested for tuples against the maxKBytes limit.
 * Fixed-size space (primarily the BufFile I/O buffer) is not
 * counted, nor do we count the variable-size memtuples array.
 * (Even though that could grow pretty large, it should be small compared
 * to the tuples proper, so this is not unreasonable.)
 *
 * The major deficiency in this approach is that it ignores palloc overhead.
 * The memory space actually allocated for a palloc chunk is always more
 * than the request size, and could be considerably more (as much as 2X
 * larger, in the current aset.c implementation).  So the space used could
 * be considerably more than maxKBytes says.
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
 * availMem to only three-quarters of what maxKBytes indicates.  This is
 * probably the right answer if anyone complains that maxKBytes is not being
 * obeyed very faithfully.
 *
 *--------------------
 */


static Tuplestorestate *tuplestore_begin_common(bool randomAccess,
						int maxKBytes);
static void dumptuples(Tuplestorestate *state);
static unsigned int getlen(Tuplestorestate *state, bool eofOK);
static void markrunend(Tuplestorestate *state);
static void *copytup_heap(Tuplestorestate *state, void *tup);
static void writetup_heap(Tuplestorestate *state, void *tup);
static void *readtup_heap(Tuplestorestate *state, unsigned int len);


/*
 *		tuplestore_begin_xxx
 *
 * Initialize for a tuple store operation.
 *
 * After calling tuplestore_begin, the caller should call tuplestore_puttuple
 * zero or more times, then call tuplestore_donestoring when all the tuples
 * have been supplied.	After donestoring, retrieve the tuples in order
 * by calling tuplestore_gettuple until it returns NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * Call tuplestore_end to terminate the operation and release memory/disk
 * space.
 */

static Tuplestorestate *
tuplestore_begin_common(bool randomAccess, int maxKBytes)
{
	Tuplestorestate *state;

	state = (Tuplestorestate *) palloc(sizeof(Tuplestorestate));

	MemSet((char *) state, 0, sizeof(Tuplestorestate));

	state->status = TSS_INITIAL;
	state->randomAccess = randomAccess;
	state->availMem = maxKBytes * 1024L;
	state->myfile = NULL;

	state->memtupcount = 0;
	if (maxKBytes > 0)
		state->memtupsize = 1024;		/* initial guess */
	else
		state->memtupsize = 1;	/* won't really need any space */
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	return state;
}

Tuplestorestate *
tuplestore_begin_heap(bool randomAccess, int maxKBytes)
{
	Tuplestorestate *state = tuplestore_begin_common(randomAccess, maxKBytes);

	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	return state;
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
}

/*
 * Accept one tuple while collecting input data.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 */
void
tuplestore_puttuple(Tuplestorestate *state, void *tuple)
{

	/*
	 * Copy the tuple.	(Must do this even in WRITEFILE case.)
	 */
	tuple = COPYTUP(state, tuple);

	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * Stash the tuple in the in-memory array.
			 */
			if (state->memtupcount >= state->memtupsize)
			{
				/* Grow the array as needed. */
				state->memtupsize *= 2;
				state->memtuples = (void **)
					repalloc(state->memtuples,
							 state->memtupsize * sizeof(void *));
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
			state->myfile = BufFileCreateTemp();
			state->status = TSS_WRITEFILE;
			dumptuples(state);
			break;
		case TSS_WRITEFILE:
			WRITETUP(state, tuple);
			break;
		default:
			elog(ERROR, "tuplestore_puttuple: invalid state");
			break;
	}
}

/*
 * All tuples have been provided; finish writing.
 */
void
tuplestore_donestoring(Tuplestorestate *state)
{
	switch (state->status)
	{
			case TSS_INITIAL:

			/*
			 * We were able to accumulate all the tuples within the
			 * allowed amount of memory.  Just set up to scan them.
			 */
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0L;
			state->markpos_eof = false;
			state->status = TSS_READMEM;
			break;
		case TSS_WRITEFILE:

			/*
			 * Write the EOF marker.
			 */
			markrunend(state);

			/*
			 * Set up for reading from tape.
			 */
			if (BufFileSeek(state->myfile, 0, 0L, SEEK_SET) != 0)
				elog(ERROR, "tuplestore_donestoring: seek(0) failed");
			state->eof_reached = false;
			state->markpos_file = 0;
			state->markpos_offset = 0L;
			state->markpos_eof = false;
			state->status = TSS_READFILE;
			break;
		default:
			elog(ERROR, "tuplestore_donestoring: invalid state");
			break;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
void *
tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free)
{
	unsigned int tuplen;
	void	   *tup;

	switch (state->status)
	{
		case TSS_READMEM:
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

		case TSS_READFILE:
			Assert(forward || state->randomAccess);
			*should_free = true;
			if (forward)
			{
				if (state->eof_reached)
					return NULL;
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
			 */
			if (state->eof_reached)
			{

				/*
				 * Seek position is pointing just past the zero tuplen at
				 * the end of file; back up to fetch last tuple's ending
				 * length word.  If seek fails we must have a completely
				 * empty file.
				 */
				if (BufFileSeek(state->myfile, 0,
								-(long) (2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
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
				if (BufFileSeek(state->myfile, 0,
								-(long) sizeof(unsigned int),
								SEEK_CUR) != 0)
					return NULL;
				tuplen = getlen(state, false);

				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (BufFileSeek(state->myfile, 0,
							 -(long) (tuplen + 2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
				{

					/*
					 * If that fails, presumably the prev tuple is the
					 * first in the file.  Back up so that it becomes next
					 * to read in forward direction (not obviously right,
					 * but that is what in-memory case does).
					 */
					if (BufFileSeek(state->myfile, 0,
								 -(long) (tuplen + sizeof(unsigned int)),
									SEEK_CUR) != 0)
						elog(ERROR, "tuplestore_gettuple: bogus tuple len in backward scan");
					return NULL;
				}
			}

			tuplen = getlen(state, false);

			/*
			 * Now we have the length of the prior tuple, back up and read
			 * it. Note: READTUP expects we are positioned after the
			 * initial length word of the tuple, so back up to that point.
			 */
			if (BufFileSeek(state->myfile, 0,
							-(long) tuplen,
							SEEK_CUR) != 0)
				elog(ERROR, "tuplestore_gettuple: bogus tuple len in backward scan");
			tup = READTUP(state, tuplen);
			return tup;

		default:
			elog(ERROR, "tuplestore_gettuple: invalid state");
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * dumptuples - remove tuples from memory and write to tape
 */
static void
dumptuples(Tuplestorestate *state)
{
	int			i;

	for (i = 0; i < state->memtupcount; i++)
		WRITETUP(state, state->memtuples[i]);
	state->memtupcount = 0;
}

/*
 * tuplestore_rescan		- rewind and replay the scan
 */
void
tuplestore_rescan(Tuplestorestate *state)
{
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_READMEM:
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0L;
			state->markpos_eof = false;
			break;
		case TSS_READFILE:
			if (BufFileSeek(state->myfile, 0, 0L, SEEK_SET) != 0)
				elog(ERROR, "tuplestore_rescan: seek(0) failed");
			state->eof_reached = false;
			state->markpos_file = 0;
			state->markpos_offset = 0L;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "tuplestore_rescan: invalid state");
			break;
	}
}

/*
 * tuplestore_markpos	- saves current position in the tuple sequence
 */
void
tuplestore_markpos(Tuplestorestate *state)
{
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_READMEM:
			state->markpos_offset = state->current;
			state->markpos_eof = state->eof_reached;
			break;
		case TSS_READFILE:
			BufFileTell(state->myfile,
						&state->markpos_file,
						&state->markpos_offset);
			state->markpos_eof = state->eof_reached;
			break;
		default:
			elog(ERROR, "tuplestore_markpos: invalid state");
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
	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_READMEM:
			state->current = (int) state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_READFILE:
			if (BufFileSeek(state->myfile,
							state->markpos_file,
							state->markpos_offset,
							SEEK_SET) != 0)
				elog(ERROR, "tuplestore_restorepos failed");
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "tuplestore_restorepos: invalid state");
			break;
	}
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplestorestate *state, bool eofOK)
{
	unsigned int len;

	if (BufFileRead(state->myfile, (void *) &len, sizeof(len)) != sizeof(len))
		elog(ERROR, "tuplestore: unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "tuplestore: unexpected end of data");
	return len;
}

static void
markrunend(Tuplestorestate *state)
{
	unsigned int len = 0;

	if (BufFileWrite(state->myfile, (void *) &len, sizeof(len)) != sizeof(len))
		elog(ERROR, "tuplestore: write failed");
}


/*
 * Routines specialized for HeapTuple case
 */

static void *
copytup_heap(Tuplestorestate *state, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;

	USEMEM(state, HEAPTUPLESIZE + tuple->t_len);
	return (void *) heap_copytuple(tuple);
}

/*
 * We don't bother to write the HeapTupleData part of the tuple.
 */

static void
writetup_heap(Tuplestorestate *state, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;
	unsigned int tuplen;

	tuplen = tuple->t_len + sizeof(tuplen);
	if (BufFileWrite(state->myfile, (void *) &tuplen,
					 sizeof(tuplen)) != sizeof(tuplen))
		elog(ERROR, "tuplestore: write failed");
	if (BufFileWrite(state->myfile, (void *) tuple->t_data,
					 tuple->t_len) != (size_t) tuple->t_len)
		elog(ERROR, "tuplestore: write failed");
	if (state->randomAccess)	/* need trailing length word? */
		if (BufFileWrite(state->myfile, (void *) &tuplen,
						 sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "tuplestore: write failed");

	FREEMEM(state, HEAPTUPLESIZE + tuple->t_len);
	heap_freetuple(tuple);
}

static void *
readtup_heap(Tuplestorestate *state, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int) + HEAPTUPLESIZE;
	HeapTuple	tuple = (HeapTuple) palloc(tuplen);

	USEMEM(state, tuplen);
	/* reconstruct the HeapTupleData portion */
	tuple->t_len = len - sizeof(unsigned int);
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_datamcxt = CurrentMemoryContext;
	tuple->t_data = (HeapTupleHeader) (((char *) tuple) + HEAPTUPLESIZE);
	/* read in the tuple proper */
	if (BufFileRead(state->myfile, (void *) tuple->t_data,
					tuple->t_len) != (size_t) tuple->t_len)
		elog(ERROR, "tuplestore: unexpected end of data");
	if (state->randomAccess)	/* need trailing length word? */
		if (BufFileRead(state->myfile, (void *) &tuplen,
						sizeof(tuplen)) != sizeof(tuplen))
			elog(ERROR, "tuplestore: unexpected end of data");
	return (void *) tuple;
}
