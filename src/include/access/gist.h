/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  The public API for GiST indexes. This API is exposed to
 *	  individuals implementing GiST indexes, so backward-incompatible
 *	  changes should be made with care.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/gist.h,v 1.59 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "access/xlog.h"
#include "access/xlogdefs.h"
#include "storage/bufpage.h"
#include "storage/off.h"
#include "utils/rel.h"

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
#define GISTNProcs						7

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

/*
 * Page opaque data in a GiST index page.
 */
#define F_LEAF				(1 << 0)
#define F_DELETED			(1 << 1)
#define F_TUPLES_DELETED	(1 << 2)

typedef XLogRecPtr GistNSN;

typedef struct GISTPageOpaqueData
{
	GistNSN		nsn;			/* this value must change on page split */
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
 * PickSplit should check spl_(r|l)datum_exists. If it is 'true',
 * that corresponding spl_(r|l)datum already defined and
 * PickSplit should use that value. PickSplit should always set
 * spl_(r|l)datum_exists to false: GiST will check value to
 * control supportng this feature by PickSplit...
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

/*
 * Vector of GISTENTRY structs; user-defined methods union and pick
 * split takes it as one of their arguments
 */
typedef struct
{
	int32		n;				/* number of elements */
	GISTENTRY	vector[1];
} GistEntryVector;

#define GEVHDRSZ	(offsetof(GistEntryVector, vector))

/*
 * macro to initialize a GISTENTRY
 */
#define gistentryinit(e, k, r, pg, o, l) \
	do { (e).key = (k); (e).rel = (r); (e).page = (pg); \
		 (e).offset = (o); (e).leafkey = (l); } while (0)

#endif   /* GIST_H */
