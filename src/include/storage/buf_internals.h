/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *    strategy.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/buf_internals.h,v 1.68 2004/02/12 15:06:56 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/backendid.h"
#include "storage/buf.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"


/* Buf Mgr constants */
/* in bufmgr.c */
extern int	Data_Descriptors;
extern int	Free_List_Descriptor;
extern int	Lookup_List_Descriptor;
extern int	Num_Descriptors;

extern int	ShowPinTrace;

/*
 * Flags for buffer descriptors
 */
#define BM_DIRTY				(1 << 0)
#define BM_VALID				(1 << 1)
#define BM_DELETED				(1 << 2)
#define BM_IO_IN_PROGRESS		(1 << 3)
#define BM_IO_ERROR				(1 << 4)
#define BM_JUST_DIRTIED			(1 << 5)
#define BM_PIN_COUNT_WAITER		(1 << 6)

typedef bits16 BufFlags;

/*
 * Buffer tag identifies which disk block the buffer contains.
 *
 * Note: the BufferTag data must be sufficient to determine where to write the
 * block, even during a "blind write" with no relcache entry.  It's possible
 * that the backend flushing the buffer doesn't even believe the relation is
 * visible yet (its xact may have started before the xact that created the
 * rel).  The storage manager must be able to cope anyway.
 */
typedef struct buftag
{
	RelFileNode rnode;
	BlockNumber blockNum;		/* blknum relative to begin of reln */
} BufferTag;

#define CLEAR_BUFFERTAG(a) \
( \
	(a)->rnode.tblNode = InvalidOid, \
	(a)->rnode.relNode = InvalidOid, \
	(a)->blockNum = InvalidBlockNumber \
)

#define INIT_BUFFERTAG(a,xx_reln,xx_blockNum) \
( \
	(a)->blockNum = (xx_blockNum), \
	(a)->rnode = (xx_reln)->rd_node \
)

#define BUFFERTAG_EQUALS(a,xx_reln,xx_blockNum) \
( \
	(a)->rnode.tblNode == (xx_reln)->rd_node.tblNode && \
	(a)->rnode.relNode == (xx_reln)->rd_node.relNode && \
	(a)->blockNum == (xx_blockNum) \
)
#define BUFFERTAGS_EQUAL(a,b) \
( \
	(a)->rnode.tblNode == (b)->rnode.tblNode && \
	(a)->rnode.relNode == (b)->rnode.relNode && \
	(a)->blockNum == (b)->blockNum \
)

/*
 *	BufferDesc -- shared buffer cache metadata for a single
 *				  shared buffer descriptor.
 */
typedef struct sbufdesc
{
	Buffer		bufNext;		/* link in freelist chain */
	SHMEM_OFFSET data;			/* pointer to data in buf pool */

	/* tag and id must be together for table lookup */
	BufferTag	tag;			/* file/block identifier */
	int			buf_id;			/* buffer's index number (from 0) */

	BufFlags	flags;			/* see bit definitions above */
	unsigned	refcount;		/* # of backends holding pins on buffer */

	LWLockId	io_in_progress_lock;	/* to wait for I/O to complete */
	LWLockId	cntx_lock;		/* to lock access to page context */

	bool		cntxDirty;		/* new way to mark block as dirty */

	/*
	 * We can't physically remove items from a disk page if another
	 * backend has the buffer pinned.  Hence, a backend may need to wait
	 * for all other pins to go away.  This is signaled by setting its own
	 * backend ID into wait_backend_id and setting flag bit
	 * BM_PIN_COUNT_WAITER. At present, there can be only one such waiter
	 * per buffer.
	 */
	BackendId	wait_backend_id;	/* backend ID of pin-count waiter */
} BufferDesc;

#define BufferDescriptorGetBuffer(bdesc) ((bdesc)->buf_id + 1)


/*
 * Each backend has its own BufferLocks[] array holding flag bits
 * showing what locks it has set on each buffer.
 *
 * We have to free these locks during ereport(ERROR)...
 */
#define BL_IO_IN_PROGRESS	(1 << 0)	/* unimplemented */
#define BL_PIN_COUNT_LOCK	(1 << 1)

/* entry for buffer hashtable */
typedef struct
{
	BufferTag	key;
	Buffer		id;
} BufferLookupEnt;

/*
 * Definitions for the buffer replacement strategy
 */
#define STRAT_LIST_UNUSED	-1
#define STRAT_LIST_B1		0
#define STRAT_LIST_T1		1
#define STRAT_LIST_T2		2
#define STRAT_LIST_B2		3
#define STRAT_NUM_LISTS		4

/*
 * The Cache Directory Block (CDB) of the Adaptive Replacement Cache (ARC)
 */
typedef struct
{
	int				prev;		/* links in the queue */
	int				next;
	int				list;		/* current list */
	BufferTag		buf_tag;	/* buffer key */
	Buffer			buf_id;		/* currently assigned data buffer */
	TransactionId	t1_xid;		/* the xid this entry went onto T1 */
} BufferStrategyCDB;

/*
 * The shared ARC control information.
 */
typedef struct
{

	int		target_T1_size;				/* What T1 size are we aiming for */
	int		listUnusedCDB;				/* All unused StrategyCDB */
	int		listHead[STRAT_NUM_LISTS];	/* ARC lists B1, T1, T2 and B2 */
	int		listTail[STRAT_NUM_LISTS];
	int		listSize[STRAT_NUM_LISTS];
	Buffer	listFreeBuffers;			/* List of unused buffers */

	long	num_lookup;					/* Some hit statistics */
	long	num_hit[STRAT_NUM_LISTS];
	time_t	stat_report;

	BufferStrategyCDB	cdb[1];			/* The cache directory */
} BufferStrategyControl;
 
/* counters in buf_init.c */
extern long int ReadBufferCount;
extern long int ReadLocalBufferCount;
extern long int BufferHitCount;
extern long int LocalBufferHitCount;
extern long int BufferFlushCount;
extern long int LocalBufferFlushCount;


/*
 * Bufmgr Interface:
 */

/* Internal routines: only called by buf.c */

/*freelist.c*/
extern void PinBuffer(BufferDesc *buf);
extern void UnpinBuffer(BufferDesc *buf);
extern BufferDesc *StrategyBufferLookup(BufferTag *tagPtr, bool recheck);
extern BufferDesc *StrategyGetBuffer(void);
extern void StrategyReplaceBuffer(BufferDesc *buf, Relation rnode, BlockNumber blockNum);
extern void StrategyInvalidateBuffer(BufferDesc *buf);
extern void StrategyHintVacuum(bool vacuum_active);
extern int StrategyDirtyBufferList(int *buffer_dirty, int max_buffers);
extern void StrategyInitialize(bool init);

/* buf_table.c */
extern void InitBufTable(int size);
extern int BufTableLookup(BufferTag *tagPtr);
extern bool BufTableInsert(BufferTag *tagPtr, Buffer buf_id);
extern bool BufTableDelete(BufferTag *tagPtr);

/* bufmgr.c */
extern BufferDesc *BufferDescriptors;
extern bits8 *BufferLocks;

/* localbuf.c */
extern BufferDesc *LocalBufferDescriptors;

extern BufferDesc *LocalBufferAlloc(Relation reln, BlockNumber blockNum,
				 bool *foundPtr);
extern void WriteLocalBuffer(Buffer buffer, bool release);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif   /* BUFMGR_INTERNALS_H */
