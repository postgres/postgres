/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf_internals.h,v 1.41 2000/10/23 04:10:14 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/buf.h"
#include "storage/lmgr.h"

/* Buf Mgr constants */
/* in bufmgr.c */
extern int	NBuffers;
extern int	Data_Descriptors;
extern int	Free_List_Descriptor;
extern int	Lookup_List_Descriptor;
extern int	Num_Descriptors;

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

typedef bits16 BufFlags;

/* long * so alignment will be correct */
typedef long **BufferBlock;

typedef struct buftag
{
	RelFileNode	rnode;
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
 * We don't need in this data any more but it allows more user
 * friendly error messages. Feel free to get rid of it
 * (and change a lot of places -:))
 */
typedef struct bufblindid
{
	char		dbname[NAMEDATALEN];	/* name of db in which buf belongs */
	char		relname[NAMEDATALEN];	/* name of reln */
}			BufferBlindId;

#define BAD_BUFFER_ID(bid) ((bid) < 1 || (bid) > NBuffers)
#define INVALID_DESCRIPTOR (-3)

/*
 *	BufferDesc -- shared buffer cache metadata for a single
 *				  shared buffer descriptor.
 *
 *		We keep the name of the database and relation in which this
 *		buffer appears in order to avoid a catalog lookup on cache
 *		flush if we don't have the reldesc in the cache.  It is also
 *		possible that the relation to which this buffer belongs is
 *		not visible to all backends at the time that it gets flushed.
 *		Dbname, relname, dbid, and relid are enough to determine where
 *		to put the buffer, for all storage managers.
 */
typedef struct sbufdesc
{
	Buffer		freeNext;		/* links for freelist chain */
	Buffer		freePrev;
	SHMEM_OFFSET data;			/* pointer to data in buf pool */

	/* tag and id must be together for table lookup to work */
	BufferTag	tag;			/* file/block identifier */
	int			buf_id;			/* maps global desc to local desc */

	BufFlags	flags;			/* see bit definitions above */
	unsigned	refcount;		/* # of times buffer is pinned */

#ifdef HAS_TEST_AND_SET
	/* can afford a dedicated lock if test-and-set locks are available */
	slock_t		io_in_progress_lock;
	slock_t		cntx_lock;		/* to lock access to page context */
#endif	 /* HAS_TEST_AND_SET */
	unsigned	r_locks;		/* # of shared locks */
	bool		ri_lock;		/* read-intent lock */
	bool		w_lock;			/* context exclusively locked */

	BufferBlindId blind;		/* was used to support blind write */

	/*
	 * When we can't delete item from page (someone else has buffer pinned)
	 * we mark buffer for cleanup by specifying appropriate for buffer
	 * content cleanup function. Buffer will be cleaned up from release
	 * buffer functions.
	 */
	void		(*CleanupFunc)(Buffer);
} BufferDesc;

/*
 * Each backend has its own BufferLocks[] array holding flag bits
 * showing what locks it has set on each buffer.
 *
 * We have to free these locks in elog(ERROR)...
 */
#define BL_IO_IN_PROGRESS	(1 << 0)	/* unimplemented */
#define BL_R_LOCK			(1 << 1)
#define BL_RI_LOCK			(1 << 2)
#define BL_W_LOCK			(1 << 3)

/*
 *	mao tracing buffer allocation
 */

/*#define BMTRACE*/
#ifdef BMTRACE

typedef struct _bmtrace
{
	int			bmt_pid;
	long		bmt_buf;
	long		bmt_dbid;
	long		bmt_relid;
	int			bmt_blkno;
	int			bmt_op;

#define BMT_NOTUSED		0
#define BMT_ALLOCFND	1
#define BMT_ALLOCNOTFND 2
#define BMT_DEALLOC		3

}			bmtrace;

#endif	 /* BMTRACE */


/*
 * Bufmgr Interface:
 */

/* Internal routines: only called by buf.c */

/*freelist.c*/
extern void AddBufferToFreelist(BufferDesc *bf);
extern void PinBuffer(BufferDesc *buf);
extern void PinBuffer_Debug(char *file, int line, BufferDesc *buf);
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
extern BufferBlock BufferBlocks;
extern long *PrivateRefCount;
extern bits8 *BufferLocks;
extern BufferTag *BufferTagLastDirtied;
extern BufferBlindId *BufferBlindLastDirtied;
extern LockRelId *BufferRelidLastDirtied;
extern bool *BufferDirtiedByMe;
extern SPINLOCK BufMgrLock;

/* localbuf.c */
extern long *LocalRefCount;
extern BufferDesc *LocalBufferDescriptors;
extern int	NLocBuffer;

extern BufferDesc *LocalBufferAlloc(Relation reln, BlockNumber blockNum,
				 bool *foundPtr);
extern int	WriteLocalBuffer(Buffer buffer, bool release);
extern int	FlushLocalBuffer(Buffer buffer, bool release);
extern void InitLocalBuffer(void);
extern void LocalBufferSync(void);
extern void ResetLocalBufferPool(void);

#endif	 /* BUFMGR_INTERNALS_H */
