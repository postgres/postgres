/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf_internals.h,v 1.61 2003/08/04 02:40:14 momjian Exp $
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
#define BM_PRIVATE				(1 << 1)
#define BM_VALID				(1 << 2)
#define BM_DELETED				(1 << 3)
#define BM_FREE					(1 << 4)
#define BM_IO_IN_PROGRESS		(1 << 5)
#define BM_IO_ERROR				(1 << 6)
#define BM_JUST_DIRTIED			(1 << 7)
#define BM_PIN_COUNT_WAITER		(1 << 8)

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

/*
 *	BufferDesc -- shared buffer cache metadata for a single
 *				  shared buffer descriptor.
 */
typedef struct sbufdesc
{
	Buffer		freeNext;		/* links for freelist chain */
	Buffer		freePrev;
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
 *	mao tracing buffer allocation
 */

/*#define BMTRACE*/

#ifdef BMTRACE

typedef struct _bmtrace
{
	int			bmt_pid;
	int			bmt_buf;
	Oid			bmt_dbid;
	Oid			bmt_relid;
	BlockNumber bmt_blkno;
	int			bmt_op;

#define BMT_NOTUSED		0
#define BMT_ALLOCFND	1
#define BMT_ALLOCNOTFND 2
#define BMT_DEALLOC		3

}	bmtrace;
#endif   /* BMTRACE */


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
extern BufferDesc *GetFreeBuffer(void);
extern void InitFreeList(bool init);

/* buf_table.c */
extern void InitBufTable(void);
extern BufferDesc *BufTableLookup(BufferTag *tagPtr);
extern bool BufTableDelete(BufferDesc *buf);
extern bool BufTableInsert(BufferDesc *buf);

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
