/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *	  strategy.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/buf_internals.h,v 1.76 2005/02/03 23:29:19 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/backendid.h"
#include "storage/buf.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/rel.h"


/*
 * Flags for buffer descriptors
 */
#define BM_DIRTY				(1 << 0)		/* data needs writing */
#define BM_VALID				(1 << 1)		/* data is valid */
#define BM_IO_IN_PROGRESS		(1 << 2)		/* read or write in
												 * progress */
#define BM_IO_ERROR				(1 << 3)		/* previous I/O failed */
#define BM_JUST_DIRTIED			(1 << 4)		/* dirtied since write
												 * started */
#define BM_PIN_COUNT_WAITER		(1 << 5)		/* have waiter for sole
												 * pin */

typedef bits16 BufFlags;

/*
 * Buffer tag identifies which disk block the buffer contains.
 *
 * Note: the BufferTag data must be sufficient to determine where to write the
 * block, without reference to pg_class or pg_tablespace entries.  It's
 * possible that the backend flushing the buffer doesn't even believe the
 * relation is visible yet (its xact may have started before the xact that
 * created the rel).  The storage manager must be able to cope anyway.
 *
 * Note: if there's any pad bytes in the struct, INIT_BUFFERTAG will have
 * to be fixed to zero them, since this struct is used as a hash key.
 */
typedef struct buftag
{
	RelFileNode rnode;			/* physical relation identifier */
	BlockNumber blockNum;		/* blknum relative to begin of reln */
} BufferTag;

#define CLEAR_BUFFERTAG(a) \
( \
	(a).rnode.spcNode = InvalidOid, \
	(a).rnode.dbNode = InvalidOid, \
	(a).rnode.relNode = InvalidOid, \
	(a).blockNum = InvalidBlockNumber \
)

#define INIT_BUFFERTAG(a,xx_reln,xx_blockNum) \
( \
	(a).rnode = (xx_reln)->rd_node, \
	(a).blockNum = (xx_blockNum) \
)

#define BUFFERTAGS_EQUAL(a,b) \
( \
	RelFileNodeEquals((a).rnode, (b).rnode) && \
	(a).blockNum == (b).blockNum \
)

/*
 *	BufferDesc -- shared descriptor/state data for a single shared buffer.
 */
typedef struct sbufdesc
{
	Buffer		bufNext;		/* link in freelist chain */
	SHMEM_OFFSET data;			/* pointer to data in buf pool */

	/* tag and id must be together for table lookup (still true?) */
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
	 * for all other pins to go away.  This is signaled by storing its own
	 * backend ID into wait_backend_id and setting flag bit
	 * BM_PIN_COUNT_WAITER. At present, there can be only one such waiter
	 * per buffer.
	 */
	BackendId	wait_backend_id;	/* backend ID of pin-count waiter */
} BufferDesc;

#define BufferDescriptorGetBuffer(bdesc) ((bdesc)->buf_id + 1)


/* in bufmgr.c */
extern BufferDesc *BufferDescriptors;

/* in localbuf.c */
extern BufferDesc *LocalBufferDescriptors;

/* counters in buf_init.c */
extern long int ReadBufferCount;
extern long int ReadLocalBufferCount;
extern long int BufferHitCount;
extern long int LocalBufferHitCount;
extern long int BufferFlushCount;
extern long int LocalBufferFlushCount;


/*
 * Internal routines: only called by bufmgr
 */

/* freelist.c */
extern BufferDesc *StrategyBufferLookup(BufferTag *tagPtr, bool recheck,
					 int *cdb_found_index);
extern BufferDesc *StrategyGetBuffer(int *cdb_replace_index);
extern void StrategyReplaceBuffer(BufferDesc *buf, BufferTag *newTag,
					  int cdb_found_index, int cdb_replace_index);
extern void StrategyInvalidateBuffer(BufferDesc *buf);
extern void StrategyHintVacuum(bool vacuum_active);
extern int StrategyDirtyBufferList(BufferDesc **buffers, BufferTag *buftags,
						int max_buffers);
extern int	StrategyShmemSize(void);
extern void StrategyInitialize(bool init);

/* buf_table.c */
extern int	BufTableShmemSize(int size);
extern void InitBufTable(int size);
extern int	BufTableLookup(BufferTag *tagPtr);
extern void BufTableInsert(BufferTag *tagPtr, int buf_id);
extern void BufTableDelete(BufferTag *tagPtr);

/* localbuf.c */
extern BufferDesc *LocalBufferAlloc(Relation reln, BlockNumber blockNum,
				 bool *foundPtr);
extern void WriteLocalBuffer(Buffer buffer, bool release);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif   /* BUFMGR_INTERNALS_H */
