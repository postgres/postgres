/*-------------------------------------------------------------------------
 *
 * hash.h
 *	  header file for postgres hash access method implementation
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hash.h
 *
 * NOTES
 *		modeled after Margo Seltzer's hash implementation for unix.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_H
#define HASH_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "catalog/pg_am_d.h"
#include "common/hashfn.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/lockdefs.h"
#include "utils/hsearch.h"
#include "utils/relcache.h"

/*
 * Mapping from hash bucket number to physical block number of bucket's
 * starting page.  Beware of multiple evaluations of argument!
 */
typedef uint32 Bucket;

#define InvalidBucket	((Bucket) 0xFFFFFFFF)

#define BUCKET_TO_BLKNO(metap,B) \
		((BlockNumber) ((B) + ((B) ? (metap)->hashm_spares[_hash_spareindex((B)+1)-1] : 0)) + 1)

/*
 * Special space for hash index pages.
 *
 * hasho_flag's LH_PAGE_TYPE bits tell us which type of page we're looking at.
 * Additional bits in the flag word are used for more transient purposes.
 *
 * To test a page's type, do (hasho_flag & LH_PAGE_TYPE) == LH_xxx_PAGE.
 * However, we ensure that each used page type has a distinct bit so that
 * we can OR together page types for uses such as the allowable-page-types
 * argument of _hash_checkpage().
 */
#define LH_UNUSED_PAGE			(0)
#define LH_OVERFLOW_PAGE		(1 << 0)
#define LH_BUCKET_PAGE			(1 << 1)
#define LH_BITMAP_PAGE			(1 << 2)
#define LH_META_PAGE			(1 << 3)
#define LH_BUCKET_BEING_POPULATED	(1 << 4)
#define LH_BUCKET_BEING_SPLIT	(1 << 5)
#define LH_BUCKET_NEEDS_SPLIT_CLEANUP	(1 << 6)
#define LH_PAGE_HAS_DEAD_TUPLES (1 << 7)

#define LH_PAGE_TYPE \
	(LH_OVERFLOW_PAGE | LH_BUCKET_PAGE | LH_BITMAP_PAGE | LH_META_PAGE)

/*
 * In an overflow page, hasho_prevblkno stores the block number of the previous
 * page in the bucket chain; in a bucket page, hasho_prevblkno stores the
 * hashm_maxbucket value as of the last time the bucket was last split, or
 * else as of the time the bucket was created.  The latter convention is used
 * to determine whether a cached copy of the metapage is too stale to be used
 * without needing to lock or pin the metapage.
 *
 * hasho_nextblkno is always the block number of the next page in the
 * bucket chain, or InvalidBlockNumber if there are no more such pages.
 */
typedef struct HashPageOpaqueData
{
	BlockNumber hasho_prevblkno;	/* see above */
	BlockNumber hasho_nextblkno;	/* see above */
	Bucket		hasho_bucket;	/* bucket number this pg belongs to */
	uint16		hasho_flag;		/* page type code + flag bits, see above */
	uint16		hasho_page_id;	/* for identification of hash indexes */
} HashPageOpaqueData;

typedef HashPageOpaqueData *HashPageOpaque;

#define HashPageGetOpaque(page) ((HashPageOpaque) PageGetSpecialPointer(page))

#define H_NEEDS_SPLIT_CLEANUP(opaque)	(((opaque)->hasho_flag & LH_BUCKET_NEEDS_SPLIT_CLEANUP) != 0)
#define H_BUCKET_BEING_SPLIT(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_SPLIT) != 0)
#define H_BUCKET_BEING_POPULATED(opaque)	(((opaque)->hasho_flag & LH_BUCKET_BEING_POPULATED) != 0)
#define H_HAS_DEAD_TUPLES(opaque)		(((opaque)->hasho_flag & LH_PAGE_HAS_DEAD_TUPLES) != 0)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 */
#define HASHO_PAGE_ID		0xFF80

typedef struct HashScanPosItem	/* what we remember about each match */
{
	ItemPointerData heapTid;	/* TID of referenced heap item */
	OffsetNumber indexOffset;	/* index item's location within page */
} HashScanPosItem;

typedef struct HashScanPosData
{
	Buffer		buf;			/* if valid, the buffer is pinned */
	BlockNumber currPage;		/* current hash index page */
	BlockNumber nextPage;		/* next overflow page */
	BlockNumber prevPage;		/* prev overflow or bucket page */

	/*
	 * The items array is always ordered in index order (ie, increasing
	 * indexoffset).  When scanning backwards it is convenient to fill the
	 * array back-to-front, so we start at the last slot and fill downwards.
	 * Hence we need both a first-valid-entry and a last-valid-entry counter.
	 * itemIndex is a cursor showing which entry was last returned to caller.
	 */
	int			firstItem;		/* first valid index in items[] */
	int			lastItem;		/* last valid index in items[] */
	int			itemIndex;		/* current index in items[] */

	HashScanPosItem items[MaxIndexTuplesPerPage];	/* MUST BE LAST */
} HashScanPosData;

#define HashScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)

#define HashScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)

#define HashScanPosInvalidate(scanpos) \
	do { \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).prevPage = InvalidBlockNumber; \
		(scanpos).firstItem = 0; \
		(scanpos).lastItem = 0; \
		(scanpos).itemIndex = 0; \
	} while (0)

/*
 *	HashScanOpaqueData is private state for a hash index scan.
 */
typedef struct HashScanOpaqueData
{
	/* Hash value of the scan key, ie, the hash key we seek */
	uint32		hashso_sk_hash;

	/* remember the buffer associated with primary bucket */
	Buffer		hashso_bucket_buf;

	/*
	 * remember the buffer associated with primary bucket page of bucket being
	 * split.  it is required during the scan of the bucket which is being
	 * populated during split operation.
	 */
	Buffer		hashso_split_bucket_buf;

	/* Whether scan starts on bucket being populated due to split */
	bool		hashso_buc_populated;

	/*
	 * Whether scanning bucket being split?  The value of this parameter is
	 * referred only when hashso_buc_populated is true.
	 */
	bool		hashso_buc_split;
	/* info about killed items if any (killedItems is NULL if never used) */
	int		   *killedItems;	/* currPos.items indexes of killed items */
	int			numKilled;		/* number of currently stored items */

	/*
	 * Identify all the matching items on a page and save them in
	 * HashScanPosData
	 */
	HashScanPosData currPos;	/* current position data */
} HashScanOpaqueData;

typedef HashScanOpaqueData *HashScanOpaque;

/*
 * Definitions for metapage.
 */

#define HASH_METAPAGE	0		/* metapage is always block 0 */

#define HASH_MAGIC		0x6440640
#define HASH_VERSION	4

/*
 * spares[] holds the number of overflow pages currently allocated at or
 * before a certain splitpoint. For example, if spares[3] = 7 then there are
 * 7 ovflpages before splitpoint 3 (compare BUCKET_TO_BLKNO macro).  The
 * value in spares[ovflpoint] increases as overflow pages are added at the
 * end of the index.  Once ovflpoint increases (ie, we have actually allocated
 * the bucket pages belonging to that splitpoint) the number of spares at the
 * prior splitpoint cannot change anymore.
 *
 * ovflpages that have been recycled for reuse can be found by looking at
 * bitmaps that are stored within ovflpages dedicated for the purpose.
 * The blknos of these bitmap pages are kept in mapp[]; nmaps is the
 * number of currently existing bitmaps.
 *
 * The limitation on the size of spares[] comes from the fact that there's
 * no point in having more than 2^32 buckets with only uint32 hashcodes.
 * (Note: The value of HASH_MAX_SPLITPOINTS which is the size of spares[] is
 * adjusted in such a way to accommodate multi phased allocation of buckets
 * after HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE).
 *
 * There is no particular upper limit on the size of mapp[], other than
 * needing to fit into the metapage.  (With 8K block size, 1024 bitmaps
 * limit us to 256 GB of overflow space...).  For smaller block size we
 * can not use 1024 bitmaps as it will lead to the meta page data crossing
 * the block size boundary.  So we use BLCKSZ to determine the maximum number
 * of bitmaps.
 */
#define HASH_MAX_BITMAPS			Min(BLCKSZ / 8, 1024)

#define HASH_SPLITPOINT_PHASE_BITS	2
#define HASH_SPLITPOINT_PHASES_PER_GRP	(1 << HASH_SPLITPOINT_PHASE_BITS)
#define HASH_SPLITPOINT_PHASE_MASK		(HASH_SPLITPOINT_PHASES_PER_GRP - 1)
#define HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE	10

/* defines max number of splitpoint phases a hash index can have */
#define HASH_MAX_SPLITPOINT_GROUP	32
#define HASH_MAX_SPLITPOINTS \
	(((HASH_MAX_SPLITPOINT_GROUP - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) * \
	  HASH_SPLITPOINT_PHASES_PER_GRP) + \
	 HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)

typedef struct HashMetaPageData
{
	uint32		hashm_magic;	/* magic no. for hash tables */
	uint32		hashm_version;	/* version ID */
	double		hashm_ntuples;	/* number of tuples stored in the table */
	uint16		hashm_ffactor;	/* target fill factor (tuples/bucket) */
	uint16		hashm_bsize;	/* index page size (bytes) */
	uint16		hashm_bmsize;	/* bitmap array size (bytes) - must be a power
								 * of 2 */
	uint16		hashm_bmshift;	/* log2(bitmap array size in BITS) */
	uint32		hashm_maxbucket;	/* ID of maximum bucket in use */
	uint32		hashm_highmask; /* mask to modulo into entire table */
	uint32		hashm_lowmask;	/* mask to modulo into lower half of table */
	uint32		hashm_ovflpoint;	/* splitpoint from which ovflpage being
									 * allocated */
	uint32		hashm_firstfree;	/* lowest-number free ovflpage (bit#) */
	uint32		hashm_nmaps;	/* number of bitmap pages */
	RegProcedure hashm_procid;	/* hash function id from pg_proc */
	uint32		hashm_spares[HASH_MAX_SPLITPOINTS]; /* spare pages before each
													 * splitpoint */
	BlockNumber hashm_mapp[HASH_MAX_BITMAPS];	/* blknos of ovfl bitmaps */
} HashMetaPageData;

typedef HashMetaPageData *HashMetaPage;

typedef struct HashOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
} HashOptions;

#define HashGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == HASH_AM_OID), \
	 (relation)->rd_options ? \
	 ((HashOptions *) (relation)->rd_options)->fillfactor :	\
	 HASH_DEFAULT_FILLFACTOR)
#define HashGetTargetPageUsage(relation) \
	(BLCKSZ * HashGetFillFactor(relation) / 100)

/*
 * Maximum size of a hash index item (it's okay to have only one per page)
 */
#define HashMaxItemSize(page) \
	MAXALIGN_DOWN(PageGetPageSize(page) - \
				  SizeOfPageHeaderData - \
				  sizeof(ItemIdData) - \
				  MAXALIGN(sizeof(HashPageOpaqueData)))

#define INDEX_MOVED_BY_SPLIT_MASK	INDEX_AM_RESERVED_BIT

#define HASH_MIN_FILLFACTOR			10
#define HASH_DEFAULT_FILLFACTOR		75

/*
 * Constants
 */
#define BYTE_TO_BIT				3	/* 2^3 bits/byte */
#define ALL_SET					((uint32) ~0)

/*
 * Bitmap pages do not contain tuples.  They do contain the standard
 * page headers and trailers; however, everything in between is a
 * giant bit array.  The number of bits that fit on a page obviously
 * depends on the page size and the header/trailer overhead.  We require
 * the number of bits per page to be a power of 2.
 */
#define BMPGSZ_BYTE(metap)		((metap)->hashm_bmsize)
#define BMPGSZ_BIT(metap)		((metap)->hashm_bmsize << BYTE_TO_BIT)
#define BMPG_SHIFT(metap)		((metap)->hashm_bmshift)
#define BMPG_MASK(metap)		(BMPGSZ_BIT(metap) - 1)

#define HashPageGetBitmap(page) \
	((uint32 *) PageGetContents(page))

#define HashGetMaxBitmapSize(page) \
	(PageGetPageSize((Page) page) - \
	 (MAXALIGN(SizeOfPageHeaderData) + MAXALIGN(sizeof(HashPageOpaqueData))))

#define HashPageGetMeta(page) \
	((HashMetaPage) PageGetContents(page))

/*
 * The number of bits in an ovflpage bitmap word.
 */
#define BITS_PER_MAP	32		/* Number of bits in uint32 */

/* Given the address of the beginning of a bit map, clear/set the nth bit */
#define CLRBIT(A, N)	((A)[(N)/BITS_PER_MAP] &= ~(1<<((N)%BITS_PER_MAP)))
#define SETBIT(A, N)	((A)[(N)/BITS_PER_MAP] |= (1<<((N)%BITS_PER_MAP)))
#define ISSET(A, N)		((A)[(N)/BITS_PER_MAP] & (1<<((N)%BITS_PER_MAP)))

/*
 * page-level and high-level locking modes (see README)
 */
#define HASH_READ		BUFFER_LOCK_SHARE
#define HASH_WRITE		BUFFER_LOCK_EXCLUSIVE
#define HASH_NOLOCK		(-1)

/*
 * When a new operator class is declared, we require that the user supply
 * us with an amproc function for hashing a key of the new type, returning
 * a 32-bit hash value.  We call this the "standard" hash function.  We
 * also allow an optional "extended" hash function which accepts a salt and
 * returns a 64-bit hash value.  This is highly recommended but, for reasons
 * of backward compatibility, optional.
 *
 * When the salt is 0, the low 32 bits of the value returned by the extended
 * hash function should match the value that would have been returned by the
 * standard hash function.
 */
#define HASHSTANDARD_PROC		1
#define HASHEXTENDED_PROC		2
#define HASHOPTIONS_PROC		3
#define HASHNProcs				3


/* public routines */

extern IndexBuildResult *hashbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);
extern void hashbuildempty(Relation index);
extern bool hashinsert(Relation rel, Datum *values, bool *isnull,
					   ItemPointer ht_ctid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern bool hashgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 hashgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern IndexScanDesc hashbeginscan(Relation rel, int nkeys, int norderbys);
extern void hashrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern void hashendscan(IndexScanDesc scan);
extern IndexBulkDeleteResult *hashbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *hashvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);
extern bytea *hashoptions(Datum reloptions, bool validate);
extern bool hashvalidate(Oid opclassoid);
extern void hashadjustmembers(Oid opfamilyoid,
							  Oid opclassoid,
							  List *operators,
							  List *functions);

/* private routines */

/* hashinsert.c */
extern void _hash_doinsert(Relation rel, IndexTuple itup, Relation heapRel,
						   bool sorted);
extern OffsetNumber _hash_pgaddtup(Relation rel, Buffer buf,
								   Size itemsize, IndexTuple itup,
								   bool appendtup);
extern void _hash_pgaddmultitup(Relation rel, Buffer buf, IndexTuple *itups,
								OffsetNumber *itup_offsets, uint16 nitups);

/* hashovfl.c */
extern Buffer _hash_addovflpage(Relation rel, Buffer metabuf, Buffer buf, bool retain_pin);
extern BlockNumber _hash_freeovflpage(Relation rel, Buffer bucketbuf, Buffer ovflbuf,
									  Buffer wbuf, IndexTuple *itups, OffsetNumber *itup_offsets,
									  Size *tups_size, uint16 nitups, BufferAccessStrategy bstrategy);
extern void _hash_initbitmapbuffer(Buffer buf, uint16 bmsize, bool initpage);
extern void _hash_squeezebucket(Relation rel,
								Bucket bucket, BlockNumber bucket_blkno,
								Buffer bucket_buf,
								BufferAccessStrategy bstrategy);
extern uint32 _hash_ovflblkno_to_bitno(HashMetaPage metap, BlockNumber ovflblkno);

/* hashpage.c */
extern Buffer _hash_getbuf(Relation rel, BlockNumber blkno,
						   int access, int flags);
extern Buffer _hash_getbuf_with_condlock_cleanup(Relation rel,
												 BlockNumber blkno, int flags);
extern HashMetaPage _hash_getcachedmetap(Relation rel, Buffer *metabuf,
										 bool force_refresh);
extern Buffer _hash_getbucketbuf_from_hashkey(Relation rel, uint32 hashkey,
											  int access,
											  HashMetaPage *cachedmetap);
extern Buffer _hash_getinitbuf(Relation rel, BlockNumber blkno);
extern void _hash_initbuf(Buffer buf, uint32 max_bucket, uint32 num_bucket,
						  uint32 flag, bool initpage);
extern Buffer _hash_getnewbuf(Relation rel, BlockNumber blkno,
							  ForkNumber forkNum);
extern Buffer _hash_getbuf_with_strategy(Relation rel, BlockNumber blkno,
										 int access, int flags,
										 BufferAccessStrategy bstrategy);
extern void _hash_relbuf(Relation rel, Buffer buf);
extern void _hash_dropbuf(Relation rel, Buffer buf);
extern void _hash_dropscanbuf(Relation rel, HashScanOpaque so);
extern uint32 _hash_init(Relation rel, double num_tuples,
						 ForkNumber forkNum);
extern void _hash_init_metabuffer(Buffer buf, double num_tuples,
								  RegProcedure procid, uint16 ffactor, bool initpage);
extern void _hash_pageinit(Page page, Size size);
extern void _hash_expandtable(Relation rel, Buffer metabuf);
extern void _hash_finish_split(Relation rel, Buffer metabuf, Buffer obuf,
							   Bucket obucket, uint32 maxbucket, uint32 highmask,
							   uint32 lowmask);

/* hashsearch.c */
extern bool _hash_next(IndexScanDesc scan, ScanDirection dir);
extern bool _hash_first(IndexScanDesc scan, ScanDirection dir);

/* hashsort.c */
typedef struct HSpool HSpool;	/* opaque struct in hashsort.c */

extern HSpool *_h_spoolinit(Relation heap, Relation index, uint32 num_buckets);
extern void _h_spooldestroy(HSpool *hspool);
extern void _h_spool(HSpool *hspool, ItemPointer self,
					 const Datum *values, const bool *isnull);
extern void _h_indexbuild(HSpool *hspool, Relation heapRel);

/* hashutil.c */
extern bool _hash_checkqual(IndexScanDesc scan, IndexTuple itup);
extern uint32 _hash_datum2hashkey(Relation rel, Datum key);
extern uint32 _hash_datum2hashkey_type(Relation rel, Datum key, Oid keytype);
extern Bucket _hash_hashkey2bucket(uint32 hashkey, uint32 maxbucket,
								   uint32 highmask, uint32 lowmask);
extern uint32 _hash_spareindex(uint32 num_bucket);
extern uint32 _hash_get_totalbuckets(uint32 splitpoint_phase);
extern void _hash_checkpage(Relation rel, Buffer buf, int flags);
extern uint32 _hash_get_indextuple_hashkey(IndexTuple itup);
extern bool _hash_convert_tuple(Relation index,
								Datum *user_values, bool *user_isnull,
								Datum *index_values, bool *index_isnull);
extern OffsetNumber _hash_binsearch(Page page, uint32 hash_value);
extern OffsetNumber _hash_binsearch_last(Page page, uint32 hash_value);
extern BlockNumber _hash_get_oldblock_from_newbucket(Relation rel, Bucket new_bucket);
extern BlockNumber _hash_get_newblock_from_oldbucket(Relation rel, Bucket old_bucket);
extern Bucket _hash_get_newbucket_from_oldbucket(Relation rel, Bucket old_bucket,
												 uint32 lowmask, uint32 maxbucket);
extern void _hash_kill_items(IndexScanDesc scan);

/* hash.c */
extern void hashbucketcleanup(Relation rel, Bucket cur_bucket,
							  Buffer bucket_buf, BlockNumber bucket_blkno,
							  BufferAccessStrategy bstrategy,
							  uint32 maxbucket, uint32 highmask, uint32 lowmask,
							  double *tuples_removed, double *num_index_tuples,
							  bool split_cleanup,
							  IndexBulkDeleteCallback callback, void *callback_state);

#endif							/* HASH_H */
