/*-------------------------------------------------------------------------
 *
 * buf_internals.h--
 *	  Internal definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf_internals.h,v 1.24 1998/07/20 16:57:10 momjian Exp $
 *
 * NOTE
 *		If BUFFERPAGE0 is defined, then 0 will be used as a
 *		valid buffer page number.
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include <storage/lmgr.h>
#include <storage/buf.h>

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

typedef struct sbufdesc BufferDesc;
typedef struct sbufdesc BufferHdr;
typedef struct buftag BufferTag;

/* long * so alignment will be correct */
typedef long **BufferBlock;

struct buftag
{
	LockRelId		relId;
	BlockNumber blockNum;		/* blknum relative to begin of reln */
};

#define CLEAR_BUFFERTAG(a) \
( \
	(a)->relId.dbId = InvalidOid, \
	(a)->relId.relId = InvalidOid, \
	(a)->blockNum = InvalidBlockNumber \
)

#define INIT_BUFFERTAG(a,xx_reln,xx_blockNum) \
( \
	(a)->blockNum = xx_blockNum, \
	(a)->relId = RelationGetLockRelId(xx_reln) \
)

#ifdef NOT_USED
#define COPY_BUFFERTAG(a,b) \
( \
	(a)->blockNum = (b)->blockNum, \
	LockRelIdAssign(*(a),*(b)) \
)

#define EQUAL_BUFFERTAG(a,b) \
( \
	((a)->blockNum == (b)->blockNum && \
   	 OID_Equal((a)->relId.relId,(b)->relId.relId)) \
)

#endif

#define BAD_BUFFER_ID(bid) ((bid<1) || (bid>(NBuffers)))
#define INVALID_DESCRIPTOR (-3)

/*
 *	struct sbufdesc -- shared buffer cache metadata for a single
 *					   shared buffer descriptor.
 *
 *		We keep the name of the database and relation in which this
 *		buffer appears in order to avoid a catalog lookup on cache
 *		flush if we don't have the reldesc in the cache.  It is also
 *		possible that the relation to which this buffer belongs is
 *		not visible to all backends at the time that it gets flushed.
 *		Dbname, relname, dbid, and relid are enough to determine where
 *		to put the buffer, for all storage managers.
 */

#define PADDED_SBUFDESC_SIZE	128

/* DO NOT CHANGE THIS NEXT STRUCTURE:
   It is used only to get padding information for the real sbufdesc structure
   It should match the sbufdesc structure exactly except for a missing sb_pad
*/
struct sbufdesc_unpadded
{
	Buffer		freeNext;
	Buffer		freePrev;
	SHMEM_OFFSET data;
	BufferTag	tag;
	int			buf_id;
	BufFlags	flags;
	unsigned	refcount;
#ifdef HAS_TEST_AND_SET
	slock_t		io_in_progress_lock;
#endif							/* HAS_TEST_AND_SET */
	char		sb_dbname[NAMEDATALEN];

	/* NOTE NO PADDING OF THE MEMBER HERE */
	char		sb_relname[NAMEDATALEN];
};

/* THE REAL STRUCTURE - the structure above must match it, minus sb_pad */
struct sbufdesc
{
	Buffer		freeNext;		/* link for freelist chain */
	Buffer		freePrev;
	SHMEM_OFFSET data;			/* pointer to data in buf pool */

	/* tag and id must be together for table lookup to work */
	BufferTag	tag;			/* file/block identifier */
	int			buf_id;			/* maps global desc to local desc */

	BufFlags	flags;			/* described below */
	unsigned	refcount;		/* # of times buffer is pinned */

#ifdef HAS_TEST_AND_SET
	/* can afford a dedicated lock if test-and-set locks are available */
	slock_t		io_in_progress_lock;
#endif							/* HAS_TEST_AND_SET */

	char		sb_dbname[NAMEDATALEN]; /* name of db in which buf belongs */

	/*
	 * I padded this structure to a power of 2 (PADDED_SBUFDESC_SIZE)
	 * because BufferDescriptorGetBuffer is called a billion times and it
	 * does an C pointer subtraction (i.e., "x - y" -> array index of x
	 * relative to y, which is calculated using division by struct size).
	 * Integer ".div" hits you for 35 cycles, as opposed to a 1-cycle
	 * "sra" ... this hack cut 10% off of the time to create the Wisconsin
	 * database! It eats up more shared memory, of course, but we're
	 * (allegedly) going to make some of these types bigger soon anyway...
	 * -pma 1/2/93
	 */

	/*
	 * please, don't take the sizeof() this member and use it for
	 * something important
	 */

	char		sb_relname[NAMEDATALEN +		/* name of reln */
				PADDED_SBUFDESC_SIZE - sizeof(struct sbufdesc_unpadded)];
};

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

#endif							/* BMTRACE */


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
extern long *LastRefCount;
extern long *CommitInfoNeedsSave;
extern SPINLOCK BufMgrLock;

/* localbuf.c */
extern long *LocalRefCount;
extern BufferDesc *LocalBufferDescriptors;
extern int	NLocBuffer;

extern BufferDesc *
LocalBufferAlloc(Relation reln, BlockNumber blockNum,
				 bool *foundPtr);
extern int	WriteLocalBuffer(Buffer buffer, bool release);
extern int	FlushLocalBuffer(Buffer buffer, bool release);
extern void InitLocalBuffer(void);
extern void LocalBufferSync(void);
extern void ResetLocalBufferPool(void);

#endif							/* BUFMGR_INTERNALS_H */
