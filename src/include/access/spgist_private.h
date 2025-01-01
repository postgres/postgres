/*-------------------------------------------------------------------------
 *
 * spgist_private.h
 *	  Private declarations for SP-GiST access method.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist_private.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_PRIVATE_H
#define SPGIST_PRIVATE_H

#include "access/itup.h"
#include "access/spgist.h"
#include "catalog/pg_am_d.h"
#include "nodes/tidbitmap.h"
#include "storage/buf.h"
#include "utils/geo_decls.h"
#include "utils/relcache.h"


typedef struct SpGistOptions
{
	int32		varlena_header_;	/* varlena header (do not touch directly!) */
	int			fillfactor;		/* page fill factor in percent (0..100) */
} SpGistOptions;

#define SpGistGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == SPGIST_AM_OID), \
	 (relation)->rd_options ? \
	 ((SpGistOptions *) (relation)->rd_options)->fillfactor : \
	 SPGIST_DEFAULT_FILLFACTOR)
#define SpGistGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - SpGistGetFillFactor(relation)) / 100)


/* SPGiST leaf tuples have one key column, optionally have included columns */
#define spgKeyColumn 0
#define spgFirstIncludeColumn 1

/* Page numbers of fixed-location pages */
#define SPGIST_METAPAGE_BLKNO	 (0)	/* metapage */
#define SPGIST_ROOT_BLKNO		 (1)	/* root for normal entries */
#define SPGIST_NULL_BLKNO		 (2)	/* root for null-value entries */
#define SPGIST_LAST_FIXED_BLKNO  SPGIST_NULL_BLKNO

#define SpGistBlockIsRoot(blkno) \
	((blkno) == SPGIST_ROOT_BLKNO || (blkno) == SPGIST_NULL_BLKNO)
#define SpGistBlockIsFixed(blkno) \
	((BlockNumber) (blkno) <= (BlockNumber) SPGIST_LAST_FIXED_BLKNO)

/*
 * Contents of page special space on SPGiST index pages
 */
typedef struct SpGistPageOpaqueData
{
	uint16		flags;			/* see bit definitions below */
	uint16		nRedirection;	/* number of redirection tuples on page */
	uint16		nPlaceholder;	/* number of placeholder tuples on page */
	/* note there's no count of either LIVE or DEAD tuples ... */
	uint16		spgist_page_id; /* for identification of SP-GiST indexes */
} SpGistPageOpaqueData;

typedef SpGistPageOpaqueData *SpGistPageOpaque;

/* Flag bits in page special space */
#define SPGIST_META			(1<<0)
#define SPGIST_DELETED		(1<<1)	/* never set, but keep for backwards
									 * compatibility */
#define SPGIST_LEAF			(1<<2)
#define SPGIST_NULLS		(1<<3)

#define SpGistPageGetOpaque(page) ((SpGistPageOpaque) PageGetSpecialPointer(page))
#define SpGistPageIsMeta(page) (SpGistPageGetOpaque(page)->flags & SPGIST_META)
#define SpGistPageIsDeleted(page) (SpGistPageGetOpaque(page)->flags & SPGIST_DELETED)
#define SpGistPageIsLeaf(page) (SpGistPageGetOpaque(page)->flags & SPGIST_LEAF)
#define SpGistPageStoresNulls(page) (SpGistPageGetOpaque(page)->flags & SPGIST_NULLS)

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 *
 * See comments above GinPageOpaqueData.
 */
#define SPGIST_PAGE_ID		0xFF82

/*
 * Each backend keeps a cache of last-used page info in its index->rd_amcache
 * area.  This is initialized from, and occasionally written back to,
 * shared storage in the index metapage.
 */
typedef struct SpGistLastUsedPage
{
	BlockNumber blkno;			/* block number, or InvalidBlockNumber */
	int			freeSpace;		/* page's free space (could be obsolete!) */
} SpGistLastUsedPage;

/* Note: indexes in cachedPage[] match flag assignments for SpGistGetBuffer */
#define SPGIST_CACHED_PAGES 8

typedef struct SpGistLUPCache
{
	SpGistLastUsedPage cachedPage[SPGIST_CACHED_PAGES];
} SpGistLUPCache;

/*
 * metapage
 */
typedef struct SpGistMetaPageData
{
	uint32		magicNumber;	/* for identity cross-check */
	SpGistLUPCache lastUsedPages;	/* shared storage of last-used info */
} SpGistMetaPageData;

#define SPGIST_MAGIC_NUMBER (0xBA0BABEE)

#define SpGistPageGetMeta(p) \
	((SpGistMetaPageData *) PageGetContents(p))

/*
 * Private state of index AM.  SpGistState is common to both insert and
 * search code; SpGistScanOpaque is for searches only.
 */

typedef struct SpGistLeafTupleData *SpGistLeafTuple;	/* forward reference */

/* Per-datatype info needed in SpGistState */
typedef struct SpGistTypeDesc
{
	Oid			type;
	int16		attlen;
	bool		attbyval;
	char		attalign;
	char		attstorage;
} SpGistTypeDesc;

typedef struct SpGistState
{
	Relation	index;			/* index we're working with */

	spgConfigOut config;		/* filled in by opclass config method */

	SpGistTypeDesc attType;		/* type of values to be indexed/restored */
	SpGistTypeDesc attLeafType; /* type of leaf-tuple values */
	SpGistTypeDesc attPrefixType;	/* type of inner-tuple prefix values */
	SpGistTypeDesc attLabelType;	/* type of node label values */

	/* leafTupDesc typically points to index's tupdesc, but not always */
	TupleDesc	leafTupDesc;	/* descriptor for leaf-level tuples */

	char	   *deadTupleStorage;	/* workspace for spgFormDeadTuple */

	TransactionId redirectXid;	/* XID to use when creating a redirect tuple */
	bool		isBuild;		/* true if doing index build */
} SpGistState;

/* Item to be re-examined later during a search */
typedef struct SpGistSearchItem
{
	pairingheap_node phNode;	/* pairing heap node */
	Datum		value;			/* value reconstructed from parent, or
								 * leafValue if isLeaf */
	SpGistLeafTuple leafTuple;	/* whole leaf tuple, if needed */
	void	   *traversalValue; /* opclass-specific traverse value */
	int			level;			/* level of items on this page */
	ItemPointerData heapPtr;	/* heap info, if heap tuple */
	bool		isNull;			/* SearchItem is NULL item */
	bool		isLeaf;			/* SearchItem is heap item */
	bool		recheck;		/* qual recheck is needed */
	bool		recheckDistances;	/* distance recheck is needed */

	/* array with numberOfOrderBys entries */
	double		distances[FLEXIBLE_ARRAY_MEMBER];
} SpGistSearchItem;

#define SizeOfSpGistSearchItem(n_distances) \
	(offsetof(SpGistSearchItem, distances) + sizeof(double) * (n_distances))

/*
 * Private state of an index scan
 */
typedef struct SpGistScanOpaqueData
{
	SpGistState state;			/* see above */
	pairingheap *scanQueue;		/* queue of to be visited items */
	MemoryContext tempCxt;		/* short-lived memory context */
	MemoryContext traversalCxt; /* single scan lifetime memory context */

	/* Control flags showing whether to search nulls and/or non-nulls */
	bool		searchNulls;	/* scan matches (all) null entries */
	bool		searchNonNulls; /* scan matches (some) non-null entries */

	/* Index quals to be passed to opclass (null-related quals removed) */
	int			numberOfKeys;	/* number of index qualifier conditions */
	ScanKey		keyData;		/* array of index qualifier descriptors */
	int			numberOfOrderBys;	/* number of ordering operators */
	int			numberOfNonNullOrderBys;	/* number of ordering operators
											 * with non-NULL arguments */
	ScanKey		orderByData;	/* array of ordering op descriptors */
	Oid		   *orderByTypes;	/* array of ordering op return types */
	int		   *nonNullOrderByOffsets;	/* array of offset of non-NULL
										 * ordering keys in the original array */
	Oid			indexCollation; /* collation of index column */

	/* Opclass defined functions: */
	FmgrInfo	innerConsistentFn;
	FmgrInfo	leafConsistentFn;

	/* Pre-allocated workspace arrays: */
	double	   *zeroDistances;
	double	   *infDistances;

	/* These fields are only used in amgetbitmap scans: */
	TIDBitmap  *tbm;			/* bitmap being filled */
	int64		ntids;			/* number of TIDs passed to bitmap */

	/* These fields are only used in amgettuple scans: */
	bool		want_itup;		/* are we reconstructing tuples? */
	TupleDesc	reconTupDesc;	/* if so, descriptor for reconstructed tuples */
	int			nPtrs;			/* number of TIDs found on current page */
	int			iPtr;			/* index for scanning through same */
	ItemPointerData heapPtrs[MaxIndexTuplesPerPage];	/* TIDs from cur page */
	bool		recheck[MaxIndexTuplesPerPage]; /* their recheck flags */
	bool		recheckDistances[MaxIndexTuplesPerPage];	/* distance recheck
															 * flags */
	HeapTuple	reconTups[MaxIndexTuplesPerPage];	/* reconstructed tuples */

	/* distances (for recheck) */
	IndexOrderByDistance *distances[MaxIndexTuplesPerPage];

	/*
	 * Note: using MaxIndexTuplesPerPage above is a bit hokey since
	 * SpGistLeafTuples aren't exactly IndexTuples; however, they are larger,
	 * so this is safe.
	 */
} SpGistScanOpaqueData;

typedef SpGistScanOpaqueData *SpGistScanOpaque;

/*
 * This struct is what we actually keep in index->rd_amcache.  It includes
 * static configuration information as well as the lastUsedPages cache.
 */
typedef struct SpGistCache
{
	spgConfigOut config;		/* filled in by opclass config method */

	SpGistTypeDesc attType;		/* type of values to be indexed/restored */
	SpGistTypeDesc attLeafType; /* type of leaf-tuple values */
	SpGistTypeDesc attPrefixType;	/* type of inner-tuple prefix values */
	SpGistTypeDesc attLabelType;	/* type of node label values */

	SpGistLUPCache lastUsedPages;	/* local storage of last-used info */
} SpGistCache;


/*
 * SPGiST tuple types.  Note: inner, leaf, and dead tuple structs
 * must have the same tupstate field in the same position!	Real inner and
 * leaf tuples always have tupstate = LIVE; if the state is something else,
 * use the SpGistDeadTuple struct to inspect the tuple.
 */

/* values of tupstate (see README for more info) */
#define SPGIST_LIVE			0	/* normal live tuple (either inner or leaf) */
#define SPGIST_REDIRECT		1	/* temporary redirection placeholder */
#define SPGIST_DEAD			2	/* dead, cannot be removed because of links */
#define SPGIST_PLACEHOLDER	3	/* placeholder, used to preserve offsets */

/*
 * SPGiST inner tuple: list of "nodes" that subdivide a set of tuples
 *
 * Inner tuple layout:
 * header/optional prefix/array of nodes, which are SpGistNodeTuples
 *
 * size and prefixSize must be multiples of MAXALIGN
 *
 * If the prefix datum is of a pass-by-value type, it is stored in its
 * Datum representation, that is its on-disk representation is of length
 * sizeof(Datum).  This is a fairly unfortunate choice, because in no other
 * place does Postgres use Datum as an on-disk representation; it creates
 * an unnecessary incompatibility between 32-bit and 64-bit builds.  But the
 * compatibility loss is mostly theoretical since MAXIMUM_ALIGNOF typically
 * differs between such builds, too.  Anyway we're stuck with it now.
 */
typedef struct SpGistInnerTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				allTheSame:1,	/* all nodes in tuple are equivalent */
				nNodes:13,		/* number of nodes within inner tuple */
				prefixSize:16;	/* size of prefix, or 0 if none */
	uint16		size;			/* total size of inner tuple */
	/* On most machines there will be a couple of wasted bytes here */
	/* prefix datum follows, then nodes */
} SpGistInnerTupleData;

typedef SpGistInnerTupleData *SpGistInnerTuple;

/* these must match largest values that fit in bit fields declared above */
#define SGITMAXNNODES		0x1FFF
#define SGITMAXPREFIXSIZE	0xFFFF
#define SGITMAXSIZE			0xFFFF

#define SGITHDRSZ			MAXALIGN(sizeof(SpGistInnerTupleData))
#define _SGITDATA(x)		(((char *) (x)) + SGITHDRSZ)
#define SGITDATAPTR(x)		((x)->prefixSize ? _SGITDATA(x) : NULL)
#define SGITDATUM(x, s)		((x)->prefixSize ? \
							 ((s)->attPrefixType.attbyval ? \
							  *(Datum *) _SGITDATA(x) : \
							  PointerGetDatum(_SGITDATA(x))) \
							 : (Datum) 0)
#define SGITNODEPTR(x)		((SpGistNodeTuple) (_SGITDATA(x) + (x)->prefixSize))

/* Macro for iterating through the nodes of an inner tuple */
#define SGITITERATE(x, i, nt)	\
	for ((i) = 0, (nt) = SGITNODEPTR(x); \
		 (i) < (x)->nNodes; \
		 (i)++, (nt) = (SpGistNodeTuple) (((char *) (nt)) + IndexTupleSize(nt)))

/*
 * SPGiST node tuple: one node within an inner tuple
 *
 * Node tuples use the same header as ordinary Postgres IndexTuples, but
 * we do not use a null bitmap, because we know there is only one column
 * so the INDEX_NULL_MASK bit suffices.  Also, pass-by-value datums are
 * stored in Datum form, the same convention as for inner tuple prefixes.
 */

typedef IndexTupleData SpGistNodeTupleData;

typedef SpGistNodeTupleData *SpGistNodeTuple;

#define SGNTHDRSZ			MAXALIGN(sizeof(SpGistNodeTupleData))
#define SGNTDATAPTR(x)		(((char *) (x)) + SGNTHDRSZ)
#define SGNTDATUM(x, s)		((s)->attLabelType.attbyval ? \
							 *(Datum *) SGNTDATAPTR(x) : \
							 PointerGetDatum(SGNTDATAPTR(x)))

/*
 * SPGiST leaf tuple: carries a leaf datum and a heap tuple TID,
 * and optionally some "included" columns.
 *
 * In the simplest case, the leaf datum is the same as the indexed value;
 * but it could also be a suffix or some other sort of delta that permits
 * reconstruction given knowledge of the prefix path traversed to get here.
 * Any included columns are stored without modification.
 *
 * A nulls bitmap is present if there are included columns AND any of the
 * datums are NULL.  We do not need a nulls bitmap for the case of a null
 * leaf datum without included columns, as we can infer whether the leaf
 * datum is null from whether the tuple is stored on a nulls page.  (This
 * provision is mostly for backwards compatibility, but it does save space
 * on 32-bit machines.)  As with other PG index tuple designs, if the nulls
 * bitmap exists then it's of size INDEX_MAX_KEYS bits regardless of the
 * actual number of attributes.  For the usual choice of INDEX_MAX_KEYS,
 * this costs nothing because of alignment considerations.
 *
 * The size field is wider than could possibly be needed for an on-disk leaf
 * tuple, but this allows us to form leaf tuples even when the datum is too
 * wide to be stored immediately, and it costs nothing because of alignment
 * considerations.
 *
 * t_info holds the nextOffset field (14 bits wide, enough for supported
 * page sizes) plus the has-nulls-bitmap flag bit; another flag bit is free.
 *
 * Normally, nextOffset links to the next tuple belonging to the same parent
 * node (which must be on the same page), or it's 0 if there is no next tuple.
 * But when the root page is a leaf page, we don't chain its tuples,
 * so nextOffset is always 0 on the root.
 *
 * size must be a multiple of MAXALIGN; also, it must be at least SGDTSIZE
 * so that the tuple can be converted to REDIRECT status later.  (This
 * restriction only adds bytes for a NULL leaf datum stored on a 32-bit
 * machine; otherwise alignment restrictions force it anyway.)
 */
typedef struct SpGistLeafTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;		/* large enough for any palloc'able value */
	uint16		t_info;			/* nextOffset, which links to the next tuple
								 * in chain, plus two flag bits */
	ItemPointerData heapPtr;	/* TID of represented heap tuple */
	/* nulls bitmap follows if the flag bit for it is set */
	/* leaf datum, then any included datums, follows on a MAXALIGN boundary */
} SpGistLeafTupleData;

/* Macros to access nextOffset and bit fields inside t_info */
#define SGLT_GET_NEXTOFFSET(spgLeafTuple) \
	((spgLeafTuple)->t_info & 0x3FFF)
#define SGLT_GET_HASNULLMASK(spgLeafTuple) \
	(((spgLeafTuple)->t_info & 0x8000) ? true : false)
#define SGLT_SET_NEXTOFFSET(spgLeafTuple, offsetNumber) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0xC000) | ((offsetNumber) & 0x3FFF))
#define SGLT_SET_HASNULLMASK(spgLeafTuple, hasnulls) \
	((spgLeafTuple)->t_info = \
	 ((spgLeafTuple)->t_info & 0x7FFF) | ((hasnulls) ? 0x8000 : 0))

#define SGLTHDRSZ(hasnulls) \
	((hasnulls) ? MAXALIGN(sizeof(SpGistLeafTupleData) + \
						   sizeof(IndexAttributeBitMapData)) : \
	 MAXALIGN(sizeof(SpGistLeafTupleData)))
#define SGLTDATAPTR(x)		(((char *) (x)) + SGLTHDRSZ(SGLT_GET_HASNULLMASK(x)))
#define SGLTDATUM(x, s)		fetch_att(SGLTDATAPTR(x), \
									  (s)->attLeafType.attbyval, \
									  (s)->attLeafType.attlen)

/*
 * SPGiST dead tuple: declaration for examining non-live tuples
 *
 * The tupstate field of this struct must match those of regular inner and
 * leaf tuples, and its size field must match a leaf tuple's.
 * Also, the pointer field must be in the same place as a leaf tuple's heapPtr
 * field, to satisfy some Asserts that we make when replacing a leaf tuple
 * with a dead tuple.
 * We don't use t_info, but it's needed to align the pointer field.
 * pointer and xid are only valid when tupstate = REDIRECT, and in some
 * cases xid can be InvalidTransactionId even then; see initSpGistState.
 */
typedef struct SpGistDeadTupleData
{
	unsigned int tupstate:2,	/* LIVE/REDIRECT/DEAD/PLACEHOLDER */
				size:30;
	uint16		t_info;			/* not used in dead tuples */
	ItemPointerData pointer;	/* redirection inside index */
	TransactionId xid;			/* ID of xact that inserted this tuple */
} SpGistDeadTupleData;

typedef SpGistDeadTupleData *SpGistDeadTuple;

#define SGDTSIZE		MAXALIGN(sizeof(SpGistDeadTupleData))

/*
 * Macros for doing free-space calculations.  Note that when adding up the
 * space needed for tuples, we always consider each tuple to need the tuple's
 * size plus sizeof(ItemIdData) (for the line pointer).  This works correctly
 * so long as tuple sizes are always maxaligned.
 */

/* Page capacity after allowing for fixed header and special space */
#define SPGIST_PAGE_CAPACITY  \
	MAXALIGN_DOWN(BLCKSZ - \
				  SizeOfPageHeaderData - \
				  MAXALIGN(sizeof(SpGistPageOpaqueData)))

/*
 * Compute free space on page, assuming that up to n placeholders can be
 * recycled if present (n should be the number of tuples to be inserted)
 */
#define SpGistPageGetFreeSpace(p, n) \
	(PageGetExactFreeSpace(p) + \
	 Min(SpGistPageGetOpaque(p)->nPlaceholder, n) * \
	 (SGDTSIZE + sizeof(ItemIdData)))

/*
 * XLOG stuff
 */

#define STORE_STATE(s, d)  \
	do { \
		(d).redirectXid = (s)->redirectXid; \
		(d).isBuild = (s)->isBuild; \
	} while(0)

/*
 * The "flags" argument for SpGistGetBuffer should be either GBUF_LEAF to
 * get a leaf page, or GBUF_INNER_PARITY(blockNumber) to get an inner
 * page in the same triple-parity group as the specified block number.
 * (Typically, this should be GBUF_INNER_PARITY(parentBlockNumber + 1)
 * to follow the rule described in spgist/README.)
 * In addition, GBUF_NULLS can be OR'd in to get a page for storage of
 * null-valued tuples.
 *
 * Note: these flag values are used as indexes into lastUsedPages.
 */
#define GBUF_LEAF				0x03
#define GBUF_INNER_PARITY(x)	((x) % 3)
#define GBUF_NULLS				0x04

#define GBUF_PARITY_MASK		0x03
#define GBUF_REQ_LEAF(flags)	(((flags) & GBUF_PARITY_MASK) == GBUF_LEAF)
#define GBUF_REQ_NULLS(flags)	((flags) & GBUF_NULLS)

/* spgutils.c */

/* reloption parameters */
#define SPGIST_MIN_FILLFACTOR			10
#define SPGIST_DEFAULT_FILLFACTOR		80

extern SpGistCache *spgGetCache(Relation index);
extern TupleDesc getSpGistTupleDesc(Relation index, SpGistTypeDesc *keyType);
extern void initSpGistState(SpGistState *state, Relation index);
extern Buffer SpGistNewBuffer(Relation index);
extern void SpGistUpdateMetaPage(Relation index);
extern Buffer SpGistGetBuffer(Relation index, int flags,
							  int needSpace, bool *isNew);
extern void SpGistSetLastUsedPage(Relation index, Buffer buffer);
extern void SpGistInitPage(Page page, uint16 f);
extern void SpGistInitBuffer(Buffer b, uint16 f);
extern void SpGistInitMetapage(Page page);
extern unsigned int SpGistGetInnerTypeSize(SpGistTypeDesc *att, Datum datum);
extern Size SpGistGetLeafTupleSize(TupleDesc tupleDescriptor,
								   const Datum *datums, const bool *isnulls);
extern SpGistLeafTuple spgFormLeafTuple(SpGistState *state,
										ItemPointer heapPtr,
										const Datum *datums, const bool *isnulls);
extern SpGistNodeTuple spgFormNodeTuple(SpGistState *state,
										Datum label, bool isnull);
extern SpGistInnerTuple spgFormInnerTuple(SpGistState *state,
										  bool hasPrefix, Datum prefix,
										  int nNodes, SpGistNodeTuple *nodes);
extern SpGistDeadTuple spgFormDeadTuple(SpGistState *state, int tupstate,
										BlockNumber blkno, OffsetNumber offnum);
extern void spgDeformLeafTuple(SpGistLeafTuple tup, TupleDesc tupleDescriptor,
							   Datum *datums, bool *isnulls,
							   bool keyColumnIsNull);
extern Datum *spgExtractNodeLabels(SpGistState *state,
								   SpGistInnerTuple innerTuple);
extern OffsetNumber SpGistPageAddNewItem(SpGistState *state, Page page,
										 Item item, Size size,
										 OffsetNumber *startOffset,
										 bool errorOK);
extern bool spgproperty(Oid index_oid, int attno,
						IndexAMProperty prop, const char *propname,
						bool *res, bool *isnull);

/* spgdoinsert.c */
extern void spgUpdateNodeLink(SpGistInnerTuple tup, int nodeN,
							  BlockNumber blkno, OffsetNumber offset);
extern void spgPageIndexMultiDelete(SpGistState *state, Page page,
									OffsetNumber *itemnos, int nitems,
									int firststate, int reststate,
									BlockNumber blkno, OffsetNumber offnum);
extern bool spgdoinsert(Relation index, SpGistState *state,
						ItemPointer heapPtr, Datum *datums, bool *isnulls);

/* spgproc.c */
extern double *spg_key_orderbys_distances(Datum key, bool isLeaf,
										  ScanKey orderbys, int norderbys);
extern BOX *box_copy(BOX *orig);

#endif							/* SPGIST_PRIVATE_H */
