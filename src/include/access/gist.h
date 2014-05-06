/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  The public API for GiST indexes. This API is exposed to
 *	  individuals implementing GiST indexes, so backward-incompatible
 *	  changes should be made with care.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/gist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/*
 * amproc indexes for GiST indexes.
 */
#define GIST_CONSISTENT_PROC			1
#define GIST_UNION_PROC					2
#define GIST_COMPRESS_PROC				3
#define GIST_DECOMPRESS_PROC			4
#define GIST_PENALTY_PROC				5
#define GIST_PICKSPLIT_PROC				6
#define GIST_EQUAL_PROC					7
#define GIST_DISTANCE_PROC				8
#define GISTNProcs						8

/*
 * strategy numbers for GiST opclasses that want to implement the old
 * RTREE behavior.
 */
#define RTLeftStrategyNumber			1
#define RTOverLeftStrategyNumber		2
#define RTOverlapStrategyNumber			3
#define RTOverRightStrategyNumber		4
#define RTRightStrategyNumber			5
#define RTSameStrategyNumber			6
#define RTContainsStrategyNumber		7		/* for @> */
#define RTContainedByStrategyNumber		8		/* for <@ */
#define RTOverBelowStrategyNumber		9
#define RTBelowStrategyNumber			10
#define RTAboveStrategyNumber			11
#define RTOverAboveStrategyNumber		12
#define RTOldContainsStrategyNumber		13		/* for old spelling of @> */
#define RTOldContainedByStrategyNumber	14		/* for old spelling of <@ */
#define RTKNNSearchStrategyNumber		15

/*
 * Page opaque data in a GiST index page.
 */
#define F_LEAF				(1 << 0)	/* leaf page */
#define F_DELETED			(1 << 1)	/* the page has been deleted */
#define F_TUPLES_DELETED	(1 << 2)	/* some tuples on the page are dead */
#define F_FOLLOW_RIGHT		(1 << 3)	/* page to the right has no downlink */

typedef XLogRecPtr GistNSN;

/*
 * For on-disk compatibility with pre-9.3 servers, NSN is stored as two
 * 32-bit fields on disk, same as LSNs.
 */
typedef PageXLogRecPtr PageGistNSN;

typedef struct GISTPageOpaqueData
{
	PageGistNSN nsn;			/* this value must change on page split */
	BlockNumber rightlink;		/* next page if any */
	uint16		flags;			/* see bit definitions above */
	uint16		gist_page_id;	/* for identification of GiST indexes */
} GISTPageOpaqueData;

typedef GISTPageOpaqueData *GISTPageOpaque;

/*
 * The page ID is for the convenience of pg_filedump and similar utilities,
 * which otherwise would have a hard time telling pages of different index
 * types apart.  It should be the last 2 bytes on the page.  This is more or
 * less "free" due to alignment considerations.
 */
#define GIST_PAGE_ID		0xFF81

/*
 * This is the Split Vector to be returned by the PickSplit method.
 * PickSplit should fill the indexes of tuples to go to the left side into
 * spl_left[], and those to go to the right into spl_right[] (note the method
 * is responsible for palloc'ing both of these arrays!).  The tuple counts
 * go into spl_nleft/spl_nright, and spl_ldatum/spl_rdatum must be set to
 * the union keys for each side.
 *
 * If spl_ldatum_exists and spl_rdatum_exists are true, then we are performing
 * a "secondary split" using a non-first index column.  In this case some
 * decisions have already been made about a page split, and the set of tuples
 * being passed to PickSplit is just the tuples about which we are undecided.
 * spl_ldatum/spl_rdatum then contain the union keys for the tuples already
 * chosen to go left or right.  Ideally the PickSplit method should take those
 * keys into account while deciding what to do with the remaining tuples, ie
 * it should try to "build out" from those unions so as to minimally expand
 * them.  If it does so, it should union the given tuples' keys into the
 * existing spl_ldatum/spl_rdatum values rather than just setting those values
 * from scratch, and then set spl_ldatum_exists/spl_rdatum_exists to false to
 * show it has done this.
 *
 * If the PickSplit method fails to clear spl_ldatum_exists/spl_rdatum_exists,
 * the core GiST code will make its own decision about how to merge the
 * secondary-split results with the previously-chosen tuples, and will then
 * recompute the union keys from scratch.  This is a workable though often not
 * optimal approach.
 */
typedef struct GIST_SPLITVEC
{
	OffsetNumber *spl_left;		/* array of entries that go left */
	int			spl_nleft;		/* size of this array */
	Datum		spl_ldatum;		/* Union of keys in spl_left */
	bool		spl_ldatum_exists;		/* true, if spl_ldatum already exists. */

	OffsetNumber *spl_right;	/* array of entries that go right */
	int			spl_nright;		/* size of the array */
	Datum		spl_rdatum;		/* Union of keys in spl_right */
	bool		spl_rdatum_exists;		/* true, if spl_rdatum already exists. */
} GIST_SPLITVEC;

/*
 * An entry on a GiST node.  Contains the key, as well as its own
 * location (rel,page,offset) which can supply the matching pointer.
 * leafkey is a flag to tell us if the entry is in a leaf node.
 */
typedef struct GISTENTRY
{
	Datum		key;
	Relation	rel;
	Page		page;
	OffsetNumber offset;
	bool		leafkey;
} GISTENTRY;

#define GistPageGetOpaque(page) ( (GISTPageOpaque) PageGetSpecialPointer(page) )

#define GistPageIsLeaf(page)	( GistPageGetOpaque(page)->flags & F_LEAF)
#define GIST_LEAF(entry) (GistPageIsLeaf((entry)->page))
#define GistPageSetLeaf(page)	( GistPageGetOpaque(page)->flags |= F_LEAF)
#define GistPageSetNonLeaf(page)	( GistPageGetOpaque(page)->flags &= ~F_LEAF)

#define GistPageIsDeleted(page) ( GistPageGetOpaque(page)->flags & F_DELETED)
#define GistPageSetDeleted(page)	( GistPageGetOpaque(page)->flags |= F_DELETED)
#define GistPageSetNonDeleted(page) ( GistPageGetOpaque(page)->flags &= ~F_DELETED)

#define GistTuplesDeleted(page) ( GistPageGetOpaque(page)->flags & F_TUPLES_DELETED)
#define GistMarkTuplesDeleted(page) ( GistPageGetOpaque(page)->flags |= F_TUPLES_DELETED)
#define GistClearTuplesDeleted(page)	( GistPageGetOpaque(page)->flags &= ~F_TUPLES_DELETED)

#define GistFollowRight(page) ( GistPageGetOpaque(page)->flags & F_FOLLOW_RIGHT)
#define GistMarkFollowRight(page) ( GistPageGetOpaque(page)->flags |= F_FOLLOW_RIGHT)
#define GistClearFollowRight(page)	( GistPageGetOpaque(page)->flags &= ~F_FOLLOW_RIGHT)

#define GistPageGetNSN(page) ( PageXLogRecPtrGet(GistPageGetOpaque(page)->nsn))
#define GistPageSetNSN(page, val) ( PageXLogRecPtrSet(GistPageGetOpaque(page)->nsn, val))

/*
 * Vector of GISTENTRY structs; user-defined methods union and picksplit
 * take it as one of their arguments
 */
typedef struct
{
	int32		n;				/* number of elements */
	GISTENTRY	vector[FLEXIBLE_ARRAY_MEMBER];
} GistEntryVector;

#define GEVHDRSZ	(offsetof(GistEntryVector, vector))

/*
 * macro to initialize a GISTENTRY
 */
#define gistentryinit(e, k, r, pg, o, l) \
	do { (e).key = (k); (e).rel = (r); (e).page = (pg); \
		 (e).offset = (o); (e).leafkey = (l); } while (0)

#endif   /* GIST_H */
