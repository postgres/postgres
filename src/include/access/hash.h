/*-------------------------------------------------------------------------
 *
 * hash.h--
 *	  header file for postgres hash access method implementation
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hash.h,v 1.19 1998/09/01 04:34:11 momjian Exp $
 *
 * NOTES
 *		modeled after Margo Seltzer's hash implementation for unix.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASH_H
#define HASH_H

#include <access/sdir.h>
#include <access/funcindex.h>
#include <storage/bufpage.h>
#include <access/relscan.h>
#include <access/itup.h>

/*
 * An overflow page is a spare page allocated for storing data whose
 * bucket doesn't have room to store it. We use overflow pages rather
 * than just splitting the bucket because there is a linear order in
 * the way we split buckets. In other words, if there isn't enough space
 * in the bucket itself, put it in an overflow page.
 *
 * Overflow page addresses are stored in form: (Splitnumber, Page offset).
 *
 * A splitnumber is the number of the generation where the table doubles
 * in size. The ovflpage's offset within the splitnumber; offsets start
 * at 1.
 *
 * We convert the stored bitmap address into a page address with the
 * macro OADDR_OF(S, O) where S is the splitnumber and O is the page
 * offset.
 */
typedef uint32 Bucket;
typedef bits16 OverflowPageAddress;
typedef uint32 SplitNumber;
typedef uint32 PageOffset;

/* A valid overflow address will always have a page offset >= 1 */
#define InvalidOvflAddress		0

#define SPLITSHIFT		11
#define SPLITMASK		0x7FF
#define SPLITNUM(N)		((SplitNumber)(((uint32)(N)) >> SPLITSHIFT))
#define OPAGENUM(N)		((PageOffset)((N) & SPLITMASK))
#define OADDR_OF(S,O)	((OverflowPageAddress)((uint32)((uint32)(S) << SPLITSHIFT) + (O)))

#define BUCKET_TO_BLKNO(B) \
		((Bucket) ((B) + ((B) ? metap->SPARES[_hash_log2((B)+1)-1] : 0)) + 1)
#define OADDR_TO_BLKNO(B)		 \
		((BlockNumber) \
		 (BUCKET_TO_BLKNO ( (1 << SPLITNUM((B))) -1 ) + OPAGENUM((B))));

/*
 * hasho_flag tells us which type of page we're looking at.  For
 * example, knowing overflow pages from bucket pages is necessary
 * information when you're deleting tuples from a page. If all the
 * tuples are deleted from an overflow page, the overflow is made
 * available to other buckets by calling _hash_freeovflpage(). If all
 * the tuples are deleted from a bucket page, no additional action is
 * necessary.
 */

#define LH_UNUSED_PAGE			(0)
#define LH_OVERFLOW_PAGE		(1 << 0)
#define LH_BUCKET_PAGE			(1 << 1)
#define LH_BITMAP_PAGE			(1 << 2)
#define LH_META_PAGE			(1 << 3)

typedef struct HashPageOpaqueData
{
	bits16		hasho_flag;		/* is this page a bucket or ovfl */
	Bucket		hasho_bucket;	/* bucket number this pg belongs to */
	OverflowPageAddress hasho_oaddr;	/* ovfl address of this ovfl pg */
	BlockNumber hasho_nextblkno;/* next ovfl blkno */
	BlockNumber hasho_prevblkno;/* previous ovfl (or bucket) blkno */
} HashPageOpaqueData;

typedef HashPageOpaqueData *HashPageOpaque;

/*
 *	ScanOpaqueData is used to remember which buffers we're currently
 *	examining in the scan.	We keep these buffers locked and pinned and
 *	recorded in the opaque entry of the scan in order to avoid doing a
 *	ReadBuffer() for every tuple in the index.	This avoids semop() calls,
 *	which are expensive.
 */

typedef struct HashScanOpaqueData
{
	Buffer		hashso_curbuf;
	Buffer		hashso_mrkbuf;
} HashScanOpaqueData;

typedef HashScanOpaqueData *HashScanOpaque;

/*
 * Definitions for metapage.
 */

#define HASH_METAPAGE	0		/* metapage is always block 0 */

#define HASH_MAGIC		0x6440640
#define HASH_VERSION	0

/*
 * NCACHED is used to set the array sizeof spares[] & bitmaps[].
 *
 * Spares[] is used to hold the number overflow pages currently
 * allocated at a certain splitpoint. For example, if spares[3] = 7
 * then there are a maximum of 7 ovflpages available at splitpoint 3.
 * The value in spares[] will change as ovflpages are added within
 * a splitpoint.
 *
 * Within a splitpoint, one can find which ovflpages are available and
 * which are used by looking at a bitmaps that are stored on the ovfl
 * pages themselves. There is at least one bitmap for every splitpoint's
 * ovflpages. Bitmaps[] contains the ovflpage addresses of the ovflpages
 * that hold the ovflpage bitmaps.
 *
 * The reason that the size is restricted to NCACHED (32) is because
 * the bitmaps are 16 bits: upper 5 represent the splitpoint, lower 11
 * indicate the page number within the splitpoint. Since there are
 * only 5 bits to store the splitpoint, there can only be 32 splitpoints.
 * Both spares[] and bitmaps[] use splitpoints as there indices, so there
 * can only be 32 of them.
 */

#define NCACHED			32


typedef struct HashMetaPageData
{
	PageHeaderData hashm_phdr;	/* pad for page header (do not use) */
	uint32		hashm_magic;	/* magic no. for hash tables */
	uint32		hashm_version;	/* version ID */
	uint32		hashm_nkeys;	/* number of keys stored in the table */
	uint16		hashm_ffactor;	/* fill factor */
	uint16		hashm_bsize;	/* bucket size (bytes) - must be a power
								 * of 2 */
	uint16		hashm_bshift;	/* bucket shift */
	uint16		hashm_bmsize;	/* bitmap array size (bytes) - must be a
								 * power of 2 */
	uint32		hashm_maxbucket;/* ID of maximum bucket in use */
	uint32		hashm_highmask; /* mask to modulo into entire table */
	uint32		hashm_lowmask;	/* mask to modulo into lower half of table */
	uint32		hashm_ovflpoint;/* pageno. from which ovflpgs being
								 * allocated */
	uint32		hashm_lastfreed;/* last ovflpage freed */
	uint32		hashm_nmaps;	/* Initial number of bitmaps */
	uint32		hashm_spares[NCACHED];	/* spare pages available at
										 * splitpoints */
	BlockNumber hashm_mapp[NCACHED];	/* blknumbers of ovfl page maps */
	RegProcedure hashm_procid;	/* hash procedure id from pg_proc */
} HashMetaPageData;

typedef HashMetaPageData *HashMetaPage;

/* Short hands for accessing structure */
#define BSHIFT			hashm_bshift
#define OVFL_POINT		hashm_ovflpoint
#define LAST_FREED		hashm_lastfreed
#define MAX_BUCKET		hashm_maxbucket
#define FFACTOR			hashm_ffactor
#define HIGH_MASK		hashm_highmask
#define LOW_MASK		hashm_lowmask
#define NKEYS			hashm_nkeys
#define SPARES			hashm_spares

extern bool BuildingHash;

typedef struct HashItemData
{
	IndexTupleData hash_itup;
} HashItemData;

typedef HashItemData *HashItem;

/*
 * Constants
 */
#define DEFAULT_FFACTOR			300
#define SPLITMAX				8
#define BYTE_TO_BIT				3		/* 2^3 bits/byte */
#define INT_TO_BYTE				2		/* 2^2 bytes/int */
#define INT_TO_BIT				5		/* 2^5 bits/int */
#define ALL_SET					((uint32) ~0)

/*
 * bitmap pages do not contain tuples.	they do contain the standard
 * page headers and trailers; however, everything in between is a
 * giant bit array.  the number of bits that fit on a page obviously
 * depends on the page size and the header/trailer overhead.
 */
#define BMPGSZ_BYTE(metap)		((metap)->hashm_bmsize)
#define BMPGSZ_BIT(metap)		((metap)->hashm_bmsize << BYTE_TO_BIT)
#define HashPageGetBitmap(pg) \
	((uint32 *) (((char *) (pg)) + DOUBLEALIGN(sizeof(PageHeaderData))))

/*
 * The number of bits in an ovflpage bitmap which
 * tells which ovflpages are empty versus in use (NOT the number of
 * bits in an overflow page *address* bitmap).
 */
#define BITS_PER_MAP	32		/* Number of bits in ovflpage bitmap */

/* Given the address of the beginning of a big map, clear/set the nth bit */
#define CLRBIT(A, N)	((A)[(N)/BITS_PER_MAP] &= ~(1<<((N)%BITS_PER_MAP)))
#define SETBIT(A, N)	((A)[(N)/BITS_PER_MAP] |= (1<<((N)%BITS_PER_MAP)))
#define ISSET(A, N)		((A)[(N)/BITS_PER_MAP] & (1<<((N)%BITS_PER_MAP)))

/*
 * page locking modes
 */
#define HASH_READ		0
#define HASH_WRITE		1

/*
 *	In general, the hash code tries to localize its knowledge about page
 *	layout to a couple of routines.  However, we need a special value to
 *	indicate "no page number" in those places where we expect page numbers.
 */

#define P_NONE			0

/*
 *	Strategy number. There's only one valid strategy for hashing: equality.
 */

#define HTEqualStrategyNumber			1
#define HTMaxStrategyNumber				1

/*
 *	When a new operator class is declared, we require that the user supply
 *	us with an amproc procudure for hashing a key of the new type.
 *	Since we only have one such proc in amproc, it's number 1.
 */

#define HASHPROC		1

/* public routines */

extern void hashbuild(Relation heap, Relation index, int natts,
		  AttrNumber *attnum, IndexStrategy istrat, uint16 pcount,
		  Datum *params, FuncIndexInfo *finfo, PredInfo *predInfo);
extern InsertIndexResult hashinsert(Relation rel, Datum *datum, char *nulls,
		   ItemPointer ht_ctid, Relation heapRel);
extern char *hashgettuple(IndexScanDesc scan, ScanDirection dir);
extern char *hashbeginscan(Relation rel, bool fromEnd, uint16 keysz,
			  ScanKey scankey);
extern void hashrescan(IndexScanDesc scan, bool fromEnd, ScanKey scankey);
extern void hashendscan(IndexScanDesc scan);
extern void hashmarkpos(IndexScanDesc scan);
extern void hashrestrpos(IndexScanDesc scan);
extern void hashdelete(Relation rel, ItemPointer tid);

/* hashfunc.c */
extern uint32 hashint2(int16 key);
extern uint32 hashint4(uint32 key);
extern uint32 hashfloat4(float32 keyp);
extern uint32 hashfloat8(float64 keyp);
extern uint32 hashoid(Oid key);
extern uint32 hashoid8(Oid *key);
extern uint32 hashchar(char key);
extern uint32 hashtext(struct varlena * key);
extern uint32 hashname(NameData *n);

/* private routines */

/* hashinsert.c */
extern InsertIndexResult _hash_doinsert(Relation rel, HashItem hitem);


/* hashovfl.c */
extern Buffer _hash_addovflpage(Relation rel, Buffer *metabufp, Buffer buf);
extern Buffer _hash_freeovflpage(Relation rel, Buffer ovflbuf);
extern int32 _hash_initbitmap(Relation rel, HashMetaPage metap, int32 pnum,
				 int32 nbits, int32 ndx);
extern void _hash_squeezebucket(Relation rel, HashMetaPage metap,
					Bucket bucket);


/* hashpage.c */
extern void _hash_metapinit(Relation rel);
extern Buffer _hash_getbuf(Relation rel, BlockNumber blkno, int access);
extern void _hash_relbuf(Relation rel, Buffer buf, int access);
extern void _hash_wrtbuf(Relation rel, Buffer buf);
extern void _hash_wrtnorelbuf(Relation rel, Buffer buf);
extern Page _hash_chgbufaccess(Relation rel, Buffer *bufp, int from_access,
				   int to_access);
extern void _hash_pageinit(Page page, Size size);
extern void _hash_pagedel(Relation rel, ItemPointer tid);
extern void _hash_expandtable(Relation rel, Buffer metabuf);


/* hashscan.c */
extern void _hash_regscan(IndexScanDesc scan);
extern void _hash_dropscan(IndexScanDesc scan);
extern void _hash_adjscans(Relation rel, ItemPointer tid);


/* hashsearch.c */
extern void _hash_search(Relation rel, int keysz, ScanKey scankey,
			 Buffer *bufP, HashMetaPage metap);
extern RetrieveIndexResult _hash_next(IndexScanDesc scan, ScanDirection dir);
extern RetrieveIndexResult _hash_first(IndexScanDesc scan, ScanDirection dir);
extern bool _hash_step(IndexScanDesc scan, Buffer *bufP, ScanDirection dir,
		   Buffer metabuf);


/* hashutil.c */
extern ScanKey _hash_mkscankey(Relation rel, IndexTuple itup,
				HashMetaPage metap);
extern void _hash_freeskey(ScanKey skey);
extern bool _hash_checkqual(IndexScanDesc scan, IndexTuple itup);
extern HashItem _hash_formitem(IndexTuple itup);
extern Bucket _hash_call(Relation rel, HashMetaPage metap, Datum key);
extern uint32 _hash_log2(uint32 num);
extern void _hash_checkpage(Page page, int flags);

#endif	 /* HASH_H */
