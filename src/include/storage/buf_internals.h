/*-------------------------------------------------------------------------
 *
 * buf_internals.h--
 *    Internal definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: buf_internals.h,v 1.3 1996/11/01 03:36:13 momjian Exp $
 *
 * NOTE
 *	If BUFFERPAGE0 is defined, then 0 will be used as a
 *	valid buffer page number.
 *
 *-------------------------------------------------------------------------
 */
#ifndef	BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/buf.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/relcache.h"

/* Buf Mgr constants */
/* in bufmgr.c */
extern int NBuffers;
extern int Data_Descriptors;
extern int Free_List_Descriptor;
extern int Lookup_List_Descriptor;
extern int Num_Descriptors;

/*
 * Flags for buffer descriptors
 */
#define BM_DIRTY   		(1 << 0)
#define BM_PRIVATE 		(1 << 1)
#define BM_VALID 		(1 << 2)
#define BM_DELETED   		(1 << 3)
#define BM_FREE			(1 << 4)
#define BM_IO_IN_PROGRESS	(1 << 5)
#define BM_IO_ERROR		(1 << 6)

typedef bits16 BufFlags;

typedef struct sbufdesc BufferDesc;
typedef struct sbufdesc BufferHdr;
typedef struct buftag BufferTag;
/* long * so alignment will be correct */
typedef long **BufferBlock;

struct buftag{
  LRelId	relId;
  BlockNumber   blockNum;  /* blknum relative to begin of reln */
};

#define CLEAR_BUFFERTAG(a)\
  (a)->relId.dbId = InvalidOid; \
  (a)->relId.relId = InvalidOid; \
  (a)->blockNum = InvalidBlockNumber

#define INIT_BUFFERTAG(a,xx_reln,xx_blockNum) \
{ \
  (a)->blockNum = xx_blockNum;\
  (a)->relId = RelationGetLRelId(xx_reln); \
}

#define COPY_BUFFERTAG(a,b)\
{ \
  (a)->blockNum = (b)->blockNum;\
  LRelIdAssign(*(a),*(b));\
}

#define EQUAL_BUFFERTAG(a,b) \
  (((a)->blockNum == (b)->blockNum) &&\
   (OID_Equal((a)->relId.relId,(b)->relId.relId)))


#define BAD_BUFFER_ID(bid) ((bid<1) || (bid>(NBuffers)))
#define INVALID_DESCRIPTOR (-3)

/*
 *  bletch hack -- anyplace that we declare space for relation or
 *  database names, we just use '16', not a symbolic constant, to
 *  specify their lengths.  BM_NAMESIZE is the length of these names,
 *  and is used in the buffer manager code.  somebody with lots of
 *  spare time should do this for all the other modules, too.
 */
#define BM_NAMESIZE	16

/*
 *  struct sbufdesc -- shared buffer cache metadata for a single
 *		       shared buffer descriptor.
 *
 *	We keep the name of the database and relation in which this
 *	buffer appears in order to avoid a catalog lookup on cache
 *	flush if we don't have the reldesc in the cache.  It is also
 *	possible that the relation to which this buffer belongs is
 *	not visible to all backends at the time that it gets flushed.
 *	Dbname, relname, dbid, and relid are enough to determine where
 *	to put the buffer, for all storage managers.
 */

struct sbufdesc {
    Buffer		freeNext;	/* link for freelist chain */
    Buffer		freePrev;
    SHMEM_OFFSET	data;		/* pointer to data in buf pool */

    /* tag and id must be together for table lookup to work */
    BufferTag		tag;		/* file/block identifier */
    int			buf_id;		/* maps global desc to local desc */

    BufFlags		flags;    	/* described below */
    int16		bufsmgr;	/* storage manager id for buffer */
    unsigned		refcount;	/* # of times buffer is pinned */

    char sb_dbname[NAMEDATALEN+1];	/* name of db in which buf belongs */
    char sb_relname[NAMEDATALEN+1];	/* name of reln */
#ifdef HAS_TEST_AND_SET
    /* can afford a dedicated lock if test-and-set locks are available */
    slock_t	io_in_progress_lock;
#endif /* HAS_TEST_AND_SET */

    /*
     * I padded this structure to a power of 2 (128 bytes on a MIPS) because
     * BufferDescriptorGetBuffer is called a billion times and it does an
     * C pointer subtraction (i.e., "x - y" -> array index of x relative
     * to y, which is calculated using division by struct size).  Integer
     * ".div" hits you for 35 cycles, as opposed to a 1-cycle "sra" ...
     * this hack cut 10% off of the time to create the Wisconsin database!
     * It eats up more shared memory, of course, but we're (allegedly)
     * going to make some of these types bigger soon anyway... -pma 1/2/93
     */

/* NO spinlock */

#if defined(PORTNAME_ultrix4)
    char		sb_pad[60];	/* no slock_t */
#endif /* mips */

/* HAS_TEST_AND_SET -- platform dependent size */

#if defined(PORTNAME_aix)
    char		sb_pad[44];	/* typedef unsigned int slock_t; */
#endif /* aix */
#if defined(PORTNAME_alpha)
    char		sb_pad[40];	/* typedef msemaphore slock_t; */
#endif /* alpha */
#if defined(PORTNAME_hpux)
    char		sb_pad[44];	/* typedef struct { int sem[4]; } slock_t; */
#endif /* hpux */
#if defined(PORTNAME_irix5)
    char		sb_pad[44];	/* typedef abilock_t slock_t; */
#endif /* irix5 */
#if defined(PORTNAME_next)
    char		sb_pad[56];	/* typedef struct mutex slock_t; */
#endif /* next */

/* HAS_TEST_AND_SET -- default 1 byte spinlock */

#if defined(PORTNAME_BSD44_derived) || \
    defined(PORTNAME_bsdi) || \
    defined(PORTNAME_i386_solaris) || \
    defined(PORTNAME_linux) || \
    defined(PORTNAME_sparc) || \
    defined(PORTNAME_sparc_solaris)
    char		sb_pad[56];	/* has slock_t */
#endif /* 1 byte slock_t */
};

/*
 *  mao tracing buffer allocation
 */

/*#define BMTRACE*/
#ifdef BMTRACE

typedef struct _bmtrace {
    int		bmt_pid;
    long	bmt_buf;
    long	bmt_dbid;
    long	bmt_relid;
    int		bmt_blkno;
    int		bmt_op;

#define BMT_NOTUSED	0
#define BMT_ALLOCFND	1
#define BMT_ALLOCNOTFND	2
#define	BMT_DEALLOC	3

} bmtrace;

#endif /* BMTRACE */


/* 
 * Bufmgr Interface:
 */

/* Internal routines: only called by buf.c */

/*freelist.c*/
extern void AddBufferToFreelist(BufferDesc *bf);
extern void PinBuffer(BufferDesc *buf);
extern void PinBuffer_Debug(char *file, int line, BufferDesc *buf);
extern void UnpinBuffer(BufferDesc *buf);
extern void UnpinBuffer_Debug(char *file, int line, BufferDesc *buf);
extern BufferDesc *GetFreeBuffer(void);
extern void InitFreeList(bool init);
extern void DBG_FreeListCheck(int nfree);

/* buf_table.c */
extern void InitBufTable(void);
extern BufferDesc *BufTableLookup(BufferTag *tagPtr);
extern bool BufTableDelete(BufferDesc *buf);
extern bool BufTableInsert(BufferDesc *buf);
extern void DBG_LookupListCheck(int nlookup);

/* bufmgr.c */
extern BufferDesc 	*BufferDescriptors;
extern BufferBlock 	BufferBlocks;
extern long		*PrivateRefCount;
extern long		*LastRefCount;
extern SPINLOCK		BufMgrLock;

/* localbuf.c */
extern long *LocalRefCount;
extern BufferDesc *LocalBufferDescriptors;
extern int NLocBuffer;

extern BufferDesc *LocalBufferAlloc(Relation reln, BlockNumber blockNum,
				    bool *foundPtr);
extern int WriteLocalBuffer(Buffer buffer, bool release);
extern int FlushLocalBuffer(Buffer buffer);
extern void InitLocalBuffer();
extern void LocalBufferSync();
extern void ResetLocalBufferPool();
     
#endif	/* BUFMGR_INTERNALS_H */
