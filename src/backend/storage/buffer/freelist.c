/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for manipulating the buffer pool's replacement strategy.
 *
 * Note: all routines in this file assume that the BufMgrLock is held
 * by the caller, so no synchronization is needed.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/freelist.c,v 1.48 2004/09/16 16:58:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include "access/xact.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"


/* GUC variable: time in seconds between statistics reports */
int			DebugSharedBuffers = 0;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;
static BufferStrategyCDB *StrategyCDB = NULL;

/* Backend-local state about whether currently vacuuming */
static bool strategy_hint_vacuum = false;
static TransactionId strategy_vacuum_xid;


#define T1_TARGET	(StrategyControl->target_T1_size)
#define B1_LENGTH	(StrategyControl->listSize[STRAT_LIST_B1])
#define T1_LENGTH	(StrategyControl->listSize[STRAT_LIST_T1])
#define T2_LENGTH	(StrategyControl->listSize[STRAT_LIST_T2])
#define B2_LENGTH	(StrategyControl->listSize[STRAT_LIST_B2])


/*
 * Macro to remove a CDB from whichever list it currently is on
 */
#define STRAT_LIST_REMOVE(cdb) \
do { \
	Assert((cdb)->list >= 0 && (cdb)->list < STRAT_NUM_LISTS);	\
	if ((cdb)->prev < 0)										\
		StrategyControl->listHead[(cdb)->list] = (cdb)->next;	\
	else														\
		StrategyCDB[(cdb)->prev].next = (cdb)->next;			\
	if ((cdb)->next < 0)										\
		StrategyControl->listTail[(cdb)->list] = (cdb)->prev;	\
	else														\
		StrategyCDB[(cdb)->next].prev = (cdb)->prev;			\
	StrategyControl->listSize[(cdb)->list]--;					\
	(cdb)->list = STRAT_LIST_UNUSED;							\
} while(0)

/*
 * Macro to add a CDB to the tail of a list (MRU position)
 */
#define STRAT_MRU_INSERT(cdb,l) \
do { \
	Assert((cdb)->list == STRAT_LIST_UNUSED);					\
	if (StrategyControl->listTail[(l)] < 0)						\
	{															\
		(cdb)->prev = (cdb)->next = -1;							\
		StrategyControl->listHead[(l)] =						\
			StrategyControl->listTail[(l)] =					\
			((cdb) - StrategyCDB);								\
	}															\
	else														\
	{															\
		(cdb)->next = -1;										\
		(cdb)->prev = StrategyControl->listTail[(l)];			\
		StrategyCDB[StrategyControl->listTail[(l)]].next =		\
			((cdb) - StrategyCDB);								\
		StrategyControl->listTail[(l)] =						\
			((cdb) - StrategyCDB);								\
	}															\
	StrategyControl->listSize[(l)]++;							\
	(cdb)->list = (l);											\
} while(0)

/*
 * Macro to add a CDB to the head of a list (LRU position)
 */
#define STRAT_LRU_INSERT(cdb,l) \
do { \
	Assert((cdb)->list == STRAT_LIST_UNUSED);					\
	if (StrategyControl->listHead[(l)] < 0)						\
	{															\
		(cdb)->prev = (cdb)->next = -1;							\
		StrategyControl->listHead[(l)] =						\
			StrategyControl->listTail[(l)] =					\
			((cdb) - StrategyCDB);								\
	}															\
	else														\
	{															\
		(cdb)->prev = -1;										\
		(cdb)->next = StrategyControl->listHead[(l)];			\
		StrategyCDB[StrategyControl->listHead[(l)]].prev =		\
			((cdb) - StrategyCDB);								\
		StrategyControl->listHead[(l)] =						\
			((cdb) - StrategyCDB);								\
	}															\
	StrategyControl->listSize[(l)]++;							\
	(cdb)->list = (l);											\
} while(0)


/*
 * Printout for use when DebugSharedBuffers is enabled
 */
static void
StrategyStatsDump(void)
{
	time_t		now = time(NULL);

	if (StrategyControl->stat_report + DebugSharedBuffers < now)
	{
		long		all_hit,
					b1_hit,
					t1_hit,
					t2_hit,
					b2_hit;
		int			id,
					t1_clean,
					t2_clean;
		ErrorContextCallback *errcxtold;

		id = StrategyControl->listHead[STRAT_LIST_T1];
		t1_clean = 0;
		while (id >= 0)
		{
			if (BufferDescriptors[StrategyCDB[id].buf_id].flags & BM_DIRTY)
				break;
			t1_clean++;
			id = StrategyCDB[id].next;
		}
		id = StrategyControl->listHead[STRAT_LIST_T2];
		t2_clean = 0;
		while (id >= 0)
		{
			if (BufferDescriptors[StrategyCDB[id].buf_id].flags & BM_DIRTY)
				break;
			t2_clean++;
			id = StrategyCDB[id].next;
		}

		if (StrategyControl->num_lookup == 0)
			all_hit = b1_hit = t1_hit = t2_hit = b2_hit = 0;
		else
		{
			b1_hit = (StrategyControl->num_hit[STRAT_LIST_B1] * 100 /
					  StrategyControl->num_lookup);
			t1_hit = (StrategyControl->num_hit[STRAT_LIST_T1] * 100 /
					  StrategyControl->num_lookup);
			t2_hit = (StrategyControl->num_hit[STRAT_LIST_T2] * 100 /
					  StrategyControl->num_lookup);
			b2_hit = (StrategyControl->num_hit[STRAT_LIST_B2] * 100 /
					  StrategyControl->num_lookup);
			all_hit = b1_hit + t1_hit + t2_hit + b2_hit;
		}

		errcxtold = error_context_stack;
		error_context_stack = NULL;
		elog(DEBUG1, "ARC T1target=%5d B1len=%5d T1len=%5d T2len=%5d B2len=%5d",
			 T1_TARGET, B1_LENGTH, T1_LENGTH, T2_LENGTH, B2_LENGTH);
		elog(DEBUG1, "ARC total   =%4ld%% B1hit=%4ld%% T1hit=%4ld%% T2hit=%4ld%% B2hit=%4ld%%",
			 all_hit, b1_hit, t1_hit, t2_hit, b2_hit);
		elog(DEBUG1, "ARC clean buffers at LRU       T1=   %5d T2=   %5d",
			 t1_clean, t2_clean);
		error_context_stack = errcxtold;

		StrategyControl->num_lookup = 0;
		StrategyControl->num_hit[STRAT_LIST_B1] = 0;
		StrategyControl->num_hit[STRAT_LIST_T1] = 0;
		StrategyControl->num_hit[STRAT_LIST_T2] = 0;
		StrategyControl->num_hit[STRAT_LIST_B2] = 0;
		StrategyControl->stat_report = now;
	}
}

/*
 * StrategyBufferLookup
 *
 *	Lookup a page request in the cache directory. A buffer is only
 *	returned for a T1 or T2 cache hit. B1 and B2 hits are just
 *	remembered here, to possibly affect the behaviour later.
 *
 *	recheck indicates we are rechecking after I/O wait; do not change
 *	internal status in this case.
 *
 *	*cdb_found_index is set to the index of the found CDB, or -1 if none.
 *	This is not intended to be used by the caller, except to pass to
 *	StrategyReplaceBuffer().
 */
BufferDesc *
StrategyBufferLookup(BufferTag *tagPtr, bool recheck,
					 int *cdb_found_index)
{
	BufferStrategyCDB *cdb;

	/* Optional stats printout */
	if (DebugSharedBuffers > 0)
		StrategyStatsDump();

	/*
	 * Count lookups
	 */
	StrategyControl->num_lookup++;

	/*
	 * Lookup the block in the shared hash table
	 */
	*cdb_found_index = BufTableLookup(tagPtr);

	/*
	 * Done if complete CDB lookup miss
	 */
	if (*cdb_found_index < 0)
		return NULL;

	/*
	 * We found a CDB
	 */
	cdb = &StrategyCDB[*cdb_found_index];

	/*
	 * Count hits
	 */
	StrategyControl->num_hit[cdb->list]++;

	/*
	 * If this is a T2 hit, we simply move the CDB to the T2 MRU position
	 * and return the found buffer.
	 *
	 * A CDB in T2 cannot have t1_vacuum set, so we needn't check.  However,
	 * if the current process is VACUUM then it doesn't promote to MRU.
	 */
	if (cdb->list == STRAT_LIST_T2)
	{
		if (!strategy_hint_vacuum)
		{
			STRAT_LIST_REMOVE(cdb);
			STRAT_MRU_INSERT(cdb, STRAT_LIST_T2);
		}

		return &BufferDescriptors[cdb->buf_id];
	}

	/*
	 * If this is a T1 hit, we move the buffer to the T2 MRU only if
	 * another transaction had read it into T1, *and* neither transaction
	 * is a VACUUM. This is required because any UPDATE or DELETE in
	 * PostgreSQL does multiple ReadBuffer(), first during the scan, later
	 * during the heap_update() or heap_delete().  Otherwise move to T1
	 * MRU.  VACUUM doesn't even get to make that happen.
	 */
	if (cdb->list == STRAT_LIST_T1)
	{
		if (!strategy_hint_vacuum)
		{
			if (!cdb->t1_vacuum &&
				!TransactionIdEquals(cdb->t1_xid, GetTopTransactionId()))
			{
				STRAT_LIST_REMOVE(cdb);
				STRAT_MRU_INSERT(cdb, STRAT_LIST_T2);
			}
			else
			{
				STRAT_LIST_REMOVE(cdb);
				STRAT_MRU_INSERT(cdb, STRAT_LIST_T1);

				/*
				 * If a non-VACUUM process references a page recently
				 * loaded by VACUUM, clear the stigma; the state will now
				 * be the same as if this process loaded it originally.
				 */
				if (cdb->t1_vacuum)
				{
					cdb->t1_xid = GetTopTransactionId();
					cdb->t1_vacuum = false;
				}
			}
		}

		return &BufferDescriptors[cdb->buf_id];
	}

	/*
	 * In the case of a recheck we don't care about B1 or B2 hits here.
	 * The bufmgr does this call only to make sure no-one faulted in the
	 * block while we where busy flushing another; we don't want to doubly
	 * adjust the T1target.
	 *
	 * Now for this really to end up as a B1 or B2 cache hit, we must have
	 * been flushing for quite some time as the block not only must have
	 * been read, but also traveled through the queue and evicted from the
	 * T cache again already.
	 *
	 * VACUUM re-reads shouldn't adjust the target either.
	 */
	if (recheck || strategy_hint_vacuum)
		return NULL;

	/*
	 * Adjust the target size of the T1 cache depending on if this is a B1
	 * or B2 hit.
	 */
	switch (cdb->list)
	{
		case STRAT_LIST_B1:

			/*
			 * B1 hit means that the T1 cache is probably too small.
			 * Adjust the T1 target size and continue below.
			 */
			T1_TARGET = Min(T1_TARGET + Max(B2_LENGTH / B1_LENGTH, 1),
							NBuffers);
			break;

		case STRAT_LIST_B2:

			/*
			 * B2 hit means that the T2 cache is probably too small.
			 * Adjust the T1 target size and continue below.
			 */
			T1_TARGET = Max(T1_TARGET - Max(B1_LENGTH / B2_LENGTH, 1), 0);
			break;

		default:
			elog(ERROR, "buffer hash table corrupted: CDB->list = %d",
				 cdb->list);
	}

	/*
	 * Even though we had seen the block in the past, its data is not
	 * currently in memory ... cache miss to the bufmgr.
	 */
	return NULL;
}


/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	this buffer must not currently be pinned.
 *
 *	*cdb_replace_index is set to the index of the candidate CDB, or -1 if
 *	none (meaning we are using a previously free buffer).  This is not
 *	intended to be used by the caller, except to pass to
 *	StrategyReplaceBuffer().
 */
BufferDesc *
StrategyGetBuffer(int *cdb_replace_index)
{
	int			cdb_id;
	BufferDesc *buf;

	if (StrategyControl->listFreeBuffers < 0)
	{
		/*
		 * We don't have a free buffer, must take one from T1 or T2.
		 * Choose based on trying to converge T1len to T1target.
		 */
		if (T1_LENGTH >= Max(1, T1_TARGET))
		{
			/*
			 * We should take the first unpinned buffer from T1.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T1];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					*cdb_replace_index = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T1);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned T1 buffer found - try T2 cache.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T2];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					*cdb_replace_index = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T2);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned buffers at all!!!
			 */
			elog(ERROR, "no unpinned buffers available");
		}
		else
		{
			/*
			 * We should take the first unpinned buffer from T2.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T2];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					*cdb_replace_index = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T2);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned T2 buffer found - try T1 cache.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T1];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					*cdb_replace_index = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T1);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned buffers at all!!!
			 */
			elog(ERROR, "no unpinned buffers available");
		}
	}
	else
	{
		/* There is a completely free buffer available - take it */

		/*
		 * Note: This code uses the side effect that a free buffer can
		 * never be pinned or dirty and therefore the call to
		 * StrategyReplaceBuffer() will happen without the bufmgr
		 * releasing the bufmgr-lock in the meantime. That means, that
		 * there will never be any reason to recheck. Otherwise we would
		 * leak shared buffers here!
		 */
		*cdb_replace_index = -1;
		buf = &BufferDescriptors[StrategyControl->listFreeBuffers];

		StrategyControl->listFreeBuffers = buf->bufNext;
		buf->bufNext = -1;

		/* Buffer in freelist cannot be pinned */
		Assert(buf->refcount == 0);
		Assert(!(buf->flags & BM_DIRTY));

		return buf;
	}

	/* not reached */
	return NULL;
}


/*
 * StrategyReplaceBuffer
 *
 *	Called by the buffer manager to inform us that he flushed a buffer
 *	and is now about to replace the content. Prior to this call,
 *	the cache algorithm still reports the buffer as in the cache. After
 *	this call we report the new block, even if IO might still need to
 *	be done to bring in the new content.
 *
 *	cdb_found_index and cdb_replace_index must be the auxiliary values
 *	returned by previous calls to StrategyBufferLookup and StrategyGetBuffer.
 */
void
StrategyReplaceBuffer(BufferDesc *buf, BufferTag *newTag,
					  int cdb_found_index, int cdb_replace_index)
{
	BufferStrategyCDB *cdb_found;
	BufferStrategyCDB *cdb_replace;

	if (cdb_found_index >= 0)
	{
		/* This must have been a ghost buffer cache hit (B1 or B2) */
		cdb_found = &StrategyCDB[cdb_found_index];

		/* Assert that the buffer remembered in cdb_found is the one */
		/* the buffer manager is currently faulting in */
		Assert(BUFFERTAGS_EQUAL(cdb_found->buf_tag, *newTag));

		if (cdb_replace_index >= 0)
		{
			/* We are satisfying it with an evicted T buffer */
			cdb_replace = &StrategyCDB[cdb_replace_index];

			/* Assert that the buffer remembered in cdb_replace is */
			/* the one the buffer manager has just evicted */
			Assert(cdb_replace->list == STRAT_LIST_T1 ||
				   cdb_replace->list == STRAT_LIST_T2);
			Assert(cdb_replace->buf_id == buf->buf_id);
			Assert(BUFFERTAGS_EQUAL(cdb_replace->buf_tag, buf->tag));

			/*
			 * Under normal circumstances we move the evicted T list entry
			 * to the corresponding B list.  However, T1 entries that
			 * exist only because of VACUUM are just thrown into the
			 * unused list instead. We don't expect them to be touched
			 * again by the VACUUM, and if we put them into B1 then VACUUM
			 * would skew T1_target adjusting.
			 */
			if (cdb_replace->t1_vacuum)
			{
				BufTableDelete(&(cdb_replace->buf_tag));
				STRAT_LIST_REMOVE(cdb_replace);
				cdb_replace->next = StrategyControl->listUnusedCDB;
				StrategyControl->listUnusedCDB = cdb_replace_index;
			}
			else
			{
				if (cdb_replace->list == STRAT_LIST_T1)
				{
					STRAT_LIST_REMOVE(cdb_replace);
					STRAT_MRU_INSERT(cdb_replace, STRAT_LIST_B1);
				}
				else
				{
					STRAT_LIST_REMOVE(cdb_replace);
					STRAT_MRU_INSERT(cdb_replace, STRAT_LIST_B2);
				}
			}
			/* And clear its block reference */
			cdb_replace->buf_id = -1;
		}
		else
		{
			/* We are satisfying it with an unused buffer */
		}

		/* Now the found B CDB gets the buffer and is moved to T2 */
		cdb_found->buf_id = buf->buf_id;
		STRAT_LIST_REMOVE(cdb_found);
		STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T2);
	}
	else
	{
		/*
		 * This was a complete cache miss, so we need to create a new CDB.
		 * The goal is to keep T1len+B1len <= c.
		 */
		if (B1_LENGTH > 0 && (T1_LENGTH + B1_LENGTH) >= NBuffers)
		{
			/* So if B1 isn't empty and T1len+B1len >= c we take B1-LRU */
			cdb_found = &StrategyCDB[StrategyControl->listHead[STRAT_LIST_B1]];

			BufTableDelete(&(cdb_found->buf_tag));
			STRAT_LIST_REMOVE(cdb_found);
		}
		else
		{
			/* Otherwise, we try to use a free one */
			if (StrategyControl->listUnusedCDB >= 0)
			{
				cdb_found = &StrategyCDB[StrategyControl->listUnusedCDB];
				StrategyControl->listUnusedCDB = cdb_found->next;
			}
			else
			{
				/* If there isn't, we take B2-LRU ... except if */
				/* T1len+B1len+T2len = c ... oh my */
				if (B2_LENGTH > 0)
					cdb_found = &StrategyCDB[StrategyControl->listHead[STRAT_LIST_B2]];
				else
					cdb_found = &StrategyCDB[StrategyControl->listHead[STRAT_LIST_B1]];

				BufTableDelete(&(cdb_found->buf_tag));
				STRAT_LIST_REMOVE(cdb_found);
			}
		}

		/* Set the CDB's buf_tag and insert it into the hash table */
		cdb_found->buf_tag = *newTag;
		BufTableInsert(&(cdb_found->buf_tag), (cdb_found - StrategyCDB));

		if (cdb_replace_index >= 0)
		{
			/*
			 * The buffer was formerly in a T list, move its CDB to the
			 * corresponding B list
			 */
			cdb_replace = &StrategyCDB[cdb_replace_index];

			Assert(cdb_replace->list == STRAT_LIST_T1 ||
				   cdb_replace->list == STRAT_LIST_T2);
			Assert(cdb_replace->buf_id == buf->buf_id);
			Assert(BUFFERTAGS_EQUAL(cdb_replace->buf_tag, buf->tag));

			if (cdb_replace->list == STRAT_LIST_T1)
			{
				STRAT_LIST_REMOVE(cdb_replace);
				STRAT_MRU_INSERT(cdb_replace, STRAT_LIST_B1);
			}
			else
			{
				STRAT_LIST_REMOVE(cdb_replace);
				STRAT_MRU_INSERT(cdb_replace, STRAT_LIST_B2);
			}
			/* And clear its block reference */
			cdb_replace->buf_id = -1;
		}
		else
		{
			/* We are satisfying it with an unused buffer */
		}

		/* Assign the buffer id to the new CDB */
		cdb_found->buf_id = buf->buf_id;

		/*
		 * Specialized VACUUM optimization. If this complete cache miss
		 * happened because vacuum needed the page, we place it at the LRU
		 * position of T1; normally it goes at the MRU position.
		 */
		if (strategy_hint_vacuum)
		{
			if (TransactionIdEquals(strategy_vacuum_xid,
									GetTopTransactionId()))
				STRAT_LRU_INSERT(cdb_found, STRAT_LIST_T1);
			else
			{
				/* VACUUM must have been aborted by error, reset flag */
				strategy_hint_vacuum = false;
				STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T1);
			}
		}
		else
			STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T1);

		/*
		 * Remember the Xid when this buffer went onto T1 to avoid a
		 * single UPDATE promoting a newcomer straight into T2. Also
		 * remember if it was loaded for VACUUM.
		 */
		cdb_found->t1_xid = GetTopTransactionId();
		cdb_found->t1_vacuum = strategy_hint_vacuum;
	}
}


/*
 * StrategyInvalidateBuffer
 *
 *	Called by the buffer manager to inform us that a buffer content
 *	is no longer valid. We simply throw away any eventual existing
 *	buffer hash entry and move the CDB and buffer to the free lists.
 */
void
StrategyInvalidateBuffer(BufferDesc *buf)
{
	int			cdb_id;
	BufferStrategyCDB *cdb;

	/* The buffer cannot be dirty or pinned */
	Assert(!(buf->flags & BM_DIRTY) || !(buf->flags & BM_VALID));
	Assert(buf->refcount == 0);

	/*
	 * Lookup the cache directory block for this buffer
	 */
	cdb_id = BufTableLookup(&(buf->tag));
	if (cdb_id < 0)
		elog(ERROR, "buffer %d not in buffer hash table", buf->buf_id);
	cdb = &StrategyCDB[cdb_id];

	/*
	 * Remove the CDB from the hashtable and the ARC queue it is currently
	 * on.
	 */
	BufTableDelete(&(cdb->buf_tag));
	STRAT_LIST_REMOVE(cdb);

	/*
	 * Clear out the CDB's buffer tag and association with the buffer and
	 * add it to the list of unused CDB's
	 */
	CLEAR_BUFFERTAG(cdb->buf_tag);
	cdb->buf_id = -1;
	cdb->next = StrategyControl->listUnusedCDB;
	StrategyControl->listUnusedCDB = cdb_id;

	/*
	 * Clear out the buffer's tag and add it to the list of currently
	 * unused buffers.	We must do this to ensure that linear scans of the
	 * buffer array don't think the buffer is valid.
	 */
	CLEAR_BUFFERTAG(buf->tag);
	buf->flags &= ~(BM_VALID | BM_DIRTY);
	buf->cntxDirty = false;
	buf->bufNext = StrategyControl->listFreeBuffers;
	StrategyControl->listFreeBuffers = buf->buf_id;
}

/*
 * StrategyHintVacuum -- tell us whether VACUUM is active
 */
void
StrategyHintVacuum(bool vacuum_active)
{
	strategy_hint_vacuum = vacuum_active;
	strategy_vacuum_xid = GetTopTransactionId();
}

/*
 * StrategyDirtyBufferList
 *
 * Returns a list of dirty buffers, in priority order for writing.
 * Note that the caller may choose not to write them all.
 *
 * The caller must beware of the possibility that a buffer is no longer dirty,
 * or even contains a different page, by the time he reaches it.  If it no
 * longer contains the same page it need not be written, even if it is (again)
 * dirty.
 *
 * Buffer pointers are stored into buffers[], and corresponding tags into
 * buftags[], both of size max_buffers.  The function returns the number of
 * buffer IDs stored.
 */
int
StrategyDirtyBufferList(BufferDesc **buffers, BufferTag *buftags,
						int max_buffers)
{
	int			num_buffer_dirty = 0;
	int			cdb_id_t1;
	int			cdb_id_t2;
	int			buf_id;
	BufferDesc *buf;

	/*
	 * Traverse the T1 and T2 list LRU to MRU in "parallel" and add all
	 * dirty buffers found in that order to the list. The ARC strategy
	 * keeps all used buffers including pinned ones in the T1 or T2 list.
	 * So we cannot miss any dirty buffers.
	 */
	cdb_id_t1 = StrategyControl->listHead[STRAT_LIST_T1];
	cdb_id_t2 = StrategyControl->listHead[STRAT_LIST_T2];

	while (cdb_id_t1 >= 0 || cdb_id_t2 >= 0)
	{
		if (cdb_id_t1 >= 0)
		{
			buf_id = StrategyCDB[cdb_id_t1].buf_id;
			buf = &BufferDescriptors[buf_id];

			if (buf->flags & BM_VALID)
			{
				if ((buf->flags & BM_DIRTY) || (buf->cntxDirty))
				{
					buffers[num_buffer_dirty] = buf;
					buftags[num_buffer_dirty] = buf->tag;
					num_buffer_dirty++;
					if (num_buffer_dirty >= max_buffers)
						break;
				}
			}

			cdb_id_t1 = StrategyCDB[cdb_id_t1].next;
		}

		if (cdb_id_t2 >= 0)
		{
			buf_id = StrategyCDB[cdb_id_t2].buf_id;
			buf = &BufferDescriptors[buf_id];

			if (buf->flags & BM_VALID)
			{
				if ((buf->flags & BM_DIRTY) || (buf->cntxDirty))
				{
					buffers[num_buffer_dirty] = buf;
					buftags[num_buffer_dirty] = buf->tag;
					num_buffer_dirty++;
					if (num_buffer_dirty >= max_buffers)
						break;
				}
			}

			cdb_id_t2 = StrategyCDB[cdb_id_t2].next;
		}
	}

	return num_buffer_dirty;
}


/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assume: All of the buffers are already building a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;
	int			i;

	/*
	 * Initialize the shared CDB lookup hashtable
	 */
	InitBufTable(NBuffers * 2);

	/*
	 * Get or create the shared strategy control block and the CDB's
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl) +
						sizeof(BufferStrategyCDB) * (NBuffers * 2 - 1),
						&found);
	StrategyCDB = &(StrategyControl->cdb[0]);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->listFreeBuffers = 0;

		/*
		 * We start off with a target T1 list size of half the available
		 * cache blocks.
		 */
		StrategyControl->target_T1_size = NBuffers / 2;

		/*
		 * Initialize B1, T1, T2 and B2 lists to be empty
		 */
		for (i = 0; i < STRAT_NUM_LISTS; i++)
		{
			StrategyControl->listHead[i] = -1;
			StrategyControl->listTail[i] = -1;
			StrategyControl->listSize[i] = 0;
			StrategyControl->num_hit[i] = 0;
		}
		StrategyControl->num_lookup = 0;
		StrategyControl->stat_report = 0;

		/*
		 * All CDB's are linked as the listUnusedCDB
		 */
		for (i = 0; i < NBuffers * 2; i++)
		{
			StrategyCDB[i].next = i + 1;
			StrategyCDB[i].list = STRAT_LIST_UNUSED;
			CLEAR_BUFFERTAG(StrategyCDB[i].buf_tag);
			StrategyCDB[i].buf_id = -1;
		}
		StrategyCDB[NBuffers * 2 - 1].next = -1;
		StrategyControl->listUnusedCDB = 0;
	}
	else
		Assert(!init);
}
