/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for manipulating the buffer pool's replacement strategy.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/freelist.c,v 1.41 2004/02/12 15:06:56 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
/*
 * OLD COMMENTS
 *
 * Data Structures:
 *		SharedFreeList is a circular queue.  Notice that this
 *		is a shared memory queue so the next/prev "ptrs" are
 *		buffer ids, not addresses.
 *
 * Sync: all routines in this file assume that the buffer
 *		semaphore has been acquired by the caller.
 */

#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "access/xact.h"
#include "miscadmin.h"

#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

static BufferStrategyControl	*StrategyControl = NULL;
static BufferStrategyCDB		*StrategyCDB = NULL;

static int		strategy_cdb_found;
static int		strategy_cdb_replace;
static int		strategy_get_from;

int				DebugSharedBuffers = 0;

static bool				strategy_hint_vacuum;
static TransactionId	strategy_vacuum_xid;


#define T1_TARGET	StrategyControl->target_T1_size
#define B1_LENGTH	StrategyControl->listSize[STRAT_LIST_B1]
#define T1_LENGTH	StrategyControl->listSize[STRAT_LIST_T1]
#define T2_LENGTH	StrategyControl->listSize[STRAT_LIST_T2]
#define B2_LENGTH	StrategyControl->listSize[STRAT_LIST_B2]


/*
 * Macro to remove a CDB from whichever list it currently is on
 */
#define	STRAT_LIST_REMOVE(cdb) \
{ \
	AssertMacro((cdb)->list >= 0 && (cdb)->list < STRAT_NUM_LISTS);		\
	if ((cdb)->prev < 0)												\
		StrategyControl->listHead[(cdb)->list] = (cdb)->next;			\
	else																\
		StrategyCDB[(cdb)->prev].next = (cdb)->next;					\
	if ((cdb)->next < 0)												\
		StrategyControl->listTail[(cdb)->list] = (cdb)->prev;			\
	else																\
		StrategyCDB[(cdb)->next].prev = (cdb)->prev;					\
	StrategyControl->listSize[(cdb)->list]--;							\
	(cdb)->list = STRAT_LIST_UNUSED;									\
}

/*
 * Macro to add a CDB to the tail of a list (MRU position)
 */
#define STRAT_MRU_INSERT(cdb,l) \
{ \
	AssertMacro((cdb)->list == STRAT_LIST_UNUSED);						\
	if (StrategyControl->listTail[(l)] < 0)								\
	{																	\
		(cdb)->prev = (cdb)->next = -1;									\
		StrategyControl->listHead[(l)] = 								\
			StrategyControl->listTail[(l)] =							\
			((cdb) - StrategyCDB);										\
	}																	\
	else																\
	{																	\
		(cdb)->next = -1;												\
		(cdb)->prev = StrategyControl->listTail[(l)];					\
		StrategyCDB[StrategyControl->listTail[(l)]].next = 				\
			((cdb) - StrategyCDB);										\
		StrategyControl->listTail[(l)] = 								\
			((cdb) - StrategyCDB);										\
	}																	\
	StrategyControl->listSize[(l)]++;									\
	(cdb)->list = (l);													\
}

/*
 * Macro to add a CDB to the head of a list (LRU position)
 */
#define STRAT_LRU_INSERT(cdb,l) \
{ \
	AssertMacro((cdb)->list == STRAT_LIST_UNUSED);						\
	if (StrategyControl->listHead[(l)] < 0)								\
	{																	\
		(cdb)->prev = (cdb)->next = -1;									\
		StrategyControl->listHead[(l)] = 								\
			StrategyControl->listTail[(l)] =							\
			((cdb) - StrategyCDB);										\
	}																	\
	else																\
	{																	\
		(cdb)->prev = -1;												\
		(cdb)->next = StrategyControl->listHead[(l)];					\
		StrategyCDB[StrategyControl->listHead[(l)]].prev = 				\
			((cdb) - StrategyCDB);										\
		StrategyControl->listHead[(l)] = 								\
			((cdb) - StrategyCDB);										\
	}																	\
	StrategyControl->listSize[(l)]++;									\
	(cdb)->list = (l);													\
}


/*
 * StrategyBufferLookup
 *
 *	Lookup a page request in the cache directory. A buffer is only
 *	returned for a T1 or T2 cache hit. B1 and B2 hits are only
 *	remembered here to later affect the behaviour.
 */
BufferDesc *
StrategyBufferLookup(BufferTag *tagPtr, bool recheck)
{
	BufferStrategyCDB  *cdb;
	time_t				now;

	if (DebugSharedBuffers > 0)
	{
		time(&now);
		if (StrategyControl->stat_report + DebugSharedBuffers < now)
		{
			long	all_hit, b1_hit, t1_hit, t2_hit, b2_hit;
			int		id, t1_clean, t2_clean;
			ErrorContextCallback	*errcxtold;

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
			{
				all_hit = b1_hit = t1_hit = t2_hit = b2_hit = 0;
			}
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
	 * Count lookups
	 */
	StrategyControl->num_lookup++;

	/*
	 * Lookup the block in the shared hash table
	 */
	strategy_cdb_found = BufTableLookup(tagPtr);

	/*
	 * Handle CDB lookup miss
	 */
	if (strategy_cdb_found < 0)
	{
		if (!recheck)
		{
			/*
			 * This is an initial lookup and we have a complete
			 * cache miss (block found nowhere). This means we
			 * remember according to the current T1 size and the
			 * target T1 size from where we take a block if we
			 * need one later.
			 */
			if (T1_LENGTH >= MAX(1, T1_TARGET))
				strategy_get_from = STRAT_LIST_T1;
			else
				strategy_get_from = STRAT_LIST_T2;
		}

		/*
		 * Do the cost accounting for vacuum
		 */
		if (VacuumCostActive)
			VacuumCostBalance += VacuumCostPageMiss;

		/* report cache miss */
		return NULL;
	}

	/*
	 * We found a CDB
	 */
	cdb = &StrategyCDB[strategy_cdb_found];

	/*
	 * Count hits
	 */
	StrategyControl->num_hit[cdb->list]++;
	if (VacuumCostActive)
		VacuumCostBalance += VacuumCostPageHit;

	/*
	 * If this is a T2 hit, we simply move the CDB to the
	 * T2 MRU position and return the found buffer.
	 */
	if (cdb->list == STRAT_LIST_T2)
	{
		STRAT_LIST_REMOVE(cdb);
		STRAT_MRU_INSERT(cdb, STRAT_LIST_T2);

		return &BufferDescriptors[cdb->buf_id];
	}

	/*
	 * If this is a T1 hit, we move the buffer to the T2 MRU
	 * only if another transaction had read it into T1. This is
	 * required because any UPDATE or DELETE in PostgreSQL does
	 * multiple ReadBuffer(), first during the scan, later during
	 * the heap_update() or heap_delete().
	 */
	if (cdb->list == STRAT_LIST_T1)
	{
		if (!TransactionIdIsCurrentTransactionId(cdb->t1_xid))
		{
			STRAT_LIST_REMOVE(cdb);
			STRAT_MRU_INSERT(cdb, STRAT_LIST_T2);
		}

		return &BufferDescriptors[cdb->buf_id];
	}

	/*
	 * In the case of a recheck we don't care about B1 or B2 hits here.
	 * The bufmgr does this call only to make sure noone faulted in the
	 * block while we where busy flushing another. Now for this really
	 * to end up as a B1 or B2 cache hit, we must have been flushing for
	 * quite some time as the block not only must have been read, but
	 * also traveled through the queue and evicted from the T cache again
	 * already. 
	 */
	if (recheck)
	{
		return NULL;
	}

	/*
	 * Adjust the target size of the T1 cache depending on if this is
	 * a B1 or B2 hit.
	 */
	switch (cdb->list)
	{
		case STRAT_LIST_B1:
			/*
			 * B1 hit means that the T1 cache is probably too
			 * small. Adjust the T1 target size and continue
			 * below.
			 */
			T1_TARGET = MIN(T1_TARGET + MAX(B2_LENGTH / B1_LENGTH, 1),
							Data_Descriptors);
			break;

		case STRAT_LIST_B2:
			/* 
			 * B2 hit means that the T2 cache is probably too
			 * small. Adjust the T1 target size and continue
			 * below.
 */
			T1_TARGET = MAX(T1_TARGET - MAX(B1_LENGTH / B2_LENGTH, 1), 0);
			break;

		default:
			elog(ERROR, "Buffer hash table corrupted - CDB on list %d found",
					cdb->list);
	}

	/*
	 * Decide where to take from if we will be out of
	 * free blocks later in StrategyGetBuffer().
	 */
	if (T1_LENGTH >= MAX(1, T1_TARGET))
		strategy_get_from = STRAT_LIST_T1;
	else
		strategy_get_from = STRAT_LIST_T2;

	/*
	 * Even if we had seen the block in the past, it's data is
	 * not currently in memory ... cache miss to the bufmgr.
	 */
	return NULL;
}


/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	this buffer must not currently be pinned. 
 */
BufferDesc *
StrategyGetBuffer(void)
{
	int				cdb_id;
	BufferDesc	   *buf;

	if (StrategyControl->listFreeBuffers < 0)
	{
		/* We don't have a free buffer, must take one from T1 or T2 */

		if (strategy_get_from == STRAT_LIST_T1)
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
					strategy_cdb_replace = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T1);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned T1 buffer found - pardon T2 cache.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T2];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					strategy_cdb_replace = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T2);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned buffers at all!!!
			 */
			elog(ERROR, "StrategyGetBuffer(): Out of unpinned buffers");
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
					strategy_cdb_replace = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T2);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned T2 buffer found - pardon T1 cache.
			 */
			cdb_id = StrategyControl->listHead[STRAT_LIST_T1];
			while (cdb_id >= 0)
			{
				buf = &BufferDescriptors[StrategyCDB[cdb_id].buf_id];
				if (buf->refcount == 0)
				{
					strategy_cdb_replace = cdb_id;
					Assert(StrategyCDB[cdb_id].list == STRAT_LIST_T1);
					return buf;
				}
				cdb_id = StrategyCDB[cdb_id].next;
			}

			/*
			 * No unpinned buffers at all!!!
			 */
			elog(ERROR, "StrategyGetBuffer(): Out of unpinned buffers");
		}
	}
	else
	{
		/* There is a completely free buffer available - take it */

		/*
		 * Note: This code uses the side effect that a free buffer
		 * can never be pinned or dirty and therefore the call to
		 * StrategyReplaceBuffer() will happen without the bufmgr
		 * releasing the bufmgr-lock in the meantime. That means,
		 * that there will never be any reason to recheck. Otherwise
		 * we would leak shared buffers here!
		 */
		strategy_cdb_replace = -1;
		buf = &BufferDescriptors[StrategyControl->listFreeBuffers];

		StrategyControl->listFreeBuffers = buf->bufNext;
		buf->bufNext = -1;

		/* Buffer of freelist cannot be pinned */
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
 *	Called by the buffer manager to inform us that he possibly flushed
 * 	a buffer and is now about to replace the content. Prior to this call,
 *	the cache algorithm still reports the buffer as in the cache. After
 *	this call we report the new block, even if IO might still need to
 *	start.
 */
void
StrategyReplaceBuffer(BufferDesc *buf, Relation rnode, BlockNumber blockNum)
{
	BufferStrategyCDB	   *cdb_found;
	BufferStrategyCDB	   *cdb_replace;

	if (strategy_cdb_found >= 0)
	{
		/* This was a ghost buffer cache hit (B1 or B2) */
		cdb_found = &StrategyCDB[strategy_cdb_found];

		/* Assert that the buffer remembered in cdb_found is the one */
		/* the buffer manager is currently faulting in */
		Assert(BUFFERTAG_EQUALS(&(cdb_found->buf_tag), rnode, blockNum));
		
		if (strategy_cdb_replace >= 0)
		{
			/* We are satisfying it with an evicted T buffer */
			cdb_replace = &StrategyCDB[strategy_cdb_replace];

			/* Assert that the buffer remembered in cdb_replace is */
			/* the one the buffer manager has just evicted */
			Assert(cdb_replace->list == STRAT_LIST_T1 || 
					cdb_replace->list == STRAT_LIST_T2);
			Assert(cdb_replace->buf_id == buf->buf_id);
			Assert(BUFFERTAGS_EQUAL(&(cdb_replace->buf_tag), &(buf->tag)));

			/* If this was a T1 buffer faulted in by vacuum, just */
			/* do not cause the CDB end up in the B1 list, so that */
			/* the vacuum scan does not affect T1_target adjusting */
			if (strategy_hint_vacuum)
			{
				BufTableDelete(&(cdb_replace->buf_tag));
				STRAT_LIST_REMOVE(cdb_replace);
				cdb_replace->buf_id = -1;
				cdb_replace->next = StrategyControl->listUnusedCDB;
				StrategyControl->listUnusedCDB = strategy_cdb_replace;
			}
			else
			{
				/* Under normal circumstances move the evicted */
				/* T list entry to it's corresponding B list */
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
			/* And clear it's block reference */
			cdb_replace->buf_id = -1;
		}
		else
		{
			/* or we satisfy it with an unused buffer */
		}

		/* Now the found B CDB get's the buffer and is moved to T2 */
		cdb_found->buf_id = buf->buf_id;
		STRAT_LIST_REMOVE(cdb_found);
		STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T2);
	}
	else
	{
		/* This was a complete cache miss, so we need to create */
		/* a new CDB. The goal is to keep T1len+B1len <= c */

		if (B1_LENGTH > 0 && (T1_LENGTH + B1_LENGTH) >= Data_Descriptors)
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

		/* Set the CDB's buf_tag and insert the hash key */
		INIT_BUFFERTAG(&(cdb_found->buf_tag), rnode, blockNum);
		BufTableInsert(&(cdb_found->buf_tag), (cdb_found - StrategyCDB));

		if (strategy_cdb_replace >= 0)
		{
			/* The buffer was formerly in a T list, move it's CDB
			 * to the corresponding B list */
			cdb_replace = &StrategyCDB[strategy_cdb_replace];

			Assert(cdb_replace->list == STRAT_LIST_T1 || 
					cdb_replace->list == STRAT_LIST_T2);
			Assert(cdb_replace->buf_id == buf->buf_id);
			Assert(BUFFERTAGS_EQUAL(&(cdb_replace->buf_tag), &(buf->tag)));

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
			/* And clear it's block reference */
			cdb_replace->buf_id = -1;
		}
		else
		{
			/* or we satisfy it with an unused buffer */
		}

		/* Assign the buffer id to the new CDB */
		cdb_found->buf_id = buf->buf_id;

		/*
		 * Specialized VACUUM optimization. If this "complete cache miss"
		 * happened because vacuum needed the page, we want it later on
		 * to be placed at the LRU instead of the MRU position of T1.
		 */
		if (strategy_hint_vacuum)
		{
			if (strategy_vacuum_xid != GetCurrentTransactionId())
			{
				strategy_hint_vacuum = false;
				STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T1);
			}
			else
				STRAT_LRU_INSERT(cdb_found, STRAT_LIST_T1);
			
		}
		else
			STRAT_MRU_INSERT(cdb_found, STRAT_LIST_T1);

		/*
		 * Remember the Xid when this buffer went onto T1 to avoid
		 * a single UPDATE promoting a newcomer straight into T2.
		 */
		cdb_found->t1_xid = GetCurrentTransactionId();
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
	int					cdb_id;
	BufferStrategyCDB  *cdb;

	/* The buffer cannot be dirty or pinned */
	Assert(!(buf->flags & BM_DIRTY));
	Assert(buf->refcount == 0);

	/*
	 * Lookup the cache directory block for this buffer
	 */
	cdb_id = BufTableLookup(&(buf->tag));
	if (cdb_id < 0)
		elog(ERROR, "StrategyInvalidateBuffer() buffer %d not in directory",
				buf->buf_id);
	cdb = &StrategyCDB[cdb_id];

	/*
	 * Remove the CDB from the hashtable and the ARC queue it is
	 * currently on.
	 */
	BufTableDelete(&(cdb->buf_tag));
	STRAT_LIST_REMOVE(cdb);

	/*
	 * Clear out the CDB's buffer tag and association with the buffer
	 * and add it to the list of unused CDB's
	 */
	CLEAR_BUFFERTAG(&(cdb->buf_tag));
	cdb->buf_id = -1;
	cdb->next = StrategyControl->listUnusedCDB;
	StrategyControl->listUnusedCDB = cdb_id;

	/*
	 * Clear out the buffers tag and add it to the list of
	 * currently unused buffers.
	 */
	CLEAR_BUFFERTAG(&(buf->tag));
	buf->bufNext = StrategyControl->listFreeBuffers;
	StrategyControl->listFreeBuffers = buf->buf_id;
}


void
StrategyHintVacuum(bool vacuum_active)
{
	strategy_hint_vacuum = vacuum_active;
	strategy_vacuum_xid = GetCurrentTransactionId();
}


int
StrategyDirtyBufferList(int *buffer_list, int max_buffers)
{
	int					num_buffer_dirty = 0;
	int					cdb_id_t1;
	int					cdb_id_t2;
	int					buf_id;
	BufferDesc		   *buf;

	/*
	 * Traverse the T1 and T2 list LRU to MRU in "parallel"
	 * and add all dirty buffers found in that order to the list.
	 * The ARC strategy keeps all used buffers including pinned ones
	 * in the T1 or T2 list. So we cannot loose any dirty buffers.
	 */
	cdb_id_t1 = StrategyControl->listHead[STRAT_LIST_T1];
	cdb_id_t2 = StrategyControl->listHead[STRAT_LIST_T2];

	while ((cdb_id_t1 >= 0 || cdb_id_t2 >= 0) && 
			num_buffer_dirty < max_buffers)
	{
		if (cdb_id_t1 >= 0)
		{
			buf_id = StrategyCDB[cdb_id_t1].buf_id;
			buf = &BufferDescriptors[buf_id];

			if (buf->flags & BM_VALID)
			{
				if ((buf->flags & BM_DIRTY) || (buf->cntxDirty))
				{
					buffer_list[num_buffer_dirty++] = buf_id;
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
					buffer_list[num_buffer_dirty++] = buf_id;
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
	bool found;
	int i;

	/*
	 * Initialize the shared CDB lookup hashtable
	 */
	InitBufTable(Data_Descriptors * 2);

	/*
	 * Get or create the shared strategy control block and the CDB's
	 */
	StrategyControl = (BufferStrategyControl *)
			ShmemInitStruct("Buffer Strategy Status",
					sizeof(BufferStrategyControl) +
					sizeof(BufferStrategyCDB) * (Data_Descriptors * 2 - 1),
					&found);
	StrategyCDB = &(StrategyControl->cdb[0]);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		/*
		 * Grab the whole linked list of free buffers for our
		 * strategy
		 */
		StrategyControl->listFreeBuffers = 0;

		/*
		 * We start off with a target T1 list size of
		 * half the available cache blocks.
		 */
		StrategyControl->target_T1_size = Data_Descriptors / 2;

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
		StrategyControl->num_lookup  = 0;
		StrategyControl->stat_report = 0;

		/*
		 * All CDB's are linked as the listUnusedCDB
		 */
		for (i = 0; i < Data_Descriptors * 2; i++)
		{
			StrategyCDB[i].next = i + 1;
			StrategyCDB[i].list = STRAT_LIST_UNUSED;
			CLEAR_BUFFERTAG(&(StrategyCDB[i].buf_tag));
			StrategyCDB[i].buf_id = -1;
		}
		StrategyCDB[Data_Descriptors * 2 - 1].next = -1;
		StrategyControl->listUnusedCDB = 0;
	}
	else
	{
		Assert(!init);
	}
}


#undef PinBuffer

/*
 * PinBuffer -- make buffer unavailable for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 * Bufmgr lock must be held by caller.
 */
void
PinBuffer(BufferDesc *buf)
{
	int			b = BufferDescriptorGetBuffer(buf) - 1;

	if (PrivateRefCount[b] == 0)
		buf->refcount++;
	PrivateRefCount[b]++;
	Assert(PrivateRefCount[b] > 0);
}

#ifdef NOT_USED
void
PinBuffer_Debug(char *file, int line, BufferDesc *buf)
{
	PinBuffer(buf);
	if (ShowPinTrace)
	{
		Buffer		buffer = BufferDescriptorGetBuffer(buf);

		fprintf(stderr, "PIN(Pin) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}
#endif

#undef UnpinBuffer

/*
 * UnpinBuffer -- make buffer available for replacement.
 *
 * This should be applied only to shared buffers, never local ones.
 * Bufmgr lock must be held by caller.
 */
void
UnpinBuffer(BufferDesc *buf)
{
	int			b = BufferDescriptorGetBuffer(buf) - 1;

	Assert(buf->refcount > 0);
	Assert(PrivateRefCount[b] > 0);
	PrivateRefCount[b]--;
	if (PrivateRefCount[b] == 0)
		buf->refcount--;

	if ((buf->flags & BM_PIN_COUNT_WAITER) != 0 &&
			 buf->refcount == 1)
	{
		/* we just released the last pin other than the waiter's */
		buf->flags &= ~BM_PIN_COUNT_WAITER;
		ProcSendSignal(buf->wait_backend_id);
	}
	else
	{
		/* do nothing */
	}
}

#ifdef NOT_USED
void
UnpinBuffer_Debug(char *file, int line, BufferDesc *buf)
{
	UnpinBuffer(buf);
	if (ShowPinTrace)
	{
		Buffer		buffer = BufferDescriptorGetBuffer(buf);

		fprintf(stderr, "UNPIN(Unpin) %ld relname = %s, blockNum = %d, \
refcount = %ld, file: %s, line: %d\n",
				buffer, buf->blind.relname, buf->tag.blockNum,
				PrivateRefCount[buffer - 1], file, line);
	}
}
#endif


