/*-------------------------------------------------------------------------
 *
 * gist.h
 *	  The public API for GiST indexes. This API is exposed to
 *	  individuals implementing GiST indexes, so backward-incompatible
 *	  changes should be made with care.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/gist.h,v 1.50 2005/10/15 02:49:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GIST_H
#define GIST_H

#include "storage/bufpage.h"
#include "storage/off.h"
#include "utils/rel.h"
#include "access/xlog.h"
#include "access/xlogdefs.h"

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
 * Page opaque data in a GiST index page.
 */
#define F_LEAF			(1 << 0)
#define F_DELETED		(1 << 1)
#define F_TUPLES_DELETED	(1 << 2)

typedef XLogRecPtr GistNSN;

typedef struct GISTPageOpaqueData
{
	uint32		flags;			/* 29 bits are unused for now */
	BlockNumber rightlink;

	/*
	 * the only meaning - change this value if page split.
	 */
	GistNSN		nsn;
} GISTPageOpaqueData;

typedef GISTPageOpaqueData *GISTPageOpaque;

/*
 * This is the Split Vector to be returned by the PickSplit method.
 */
typedef struct GIST_SPLITVEC
{
	OffsetNumber *spl_left;		/* array of entries that go left */
	int			spl_nleft;		/* size of this array */
	Datum		spl_ldatum;		/* Union of keys in spl_left */
	Datum		spl_lattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * spl_left */
	int			spl_lattrsize[INDEX_MAX_KEYS];
	bool		spl_lisnull[INDEX_MAX_KEYS];
	bool		spl_leftvalid;

	OffsetNumber *spl_right;	/* array of entries that go right */
	int			spl_nright;		/* size of the array */
	Datum		spl_rdatum;		/* Union of keys in spl_right */
	Datum		spl_rattr[INDEX_MAX_KEYS];		/* Union of subkeys in
												 * spl_right */
	int			spl_rattrsize[INDEX_MAX_KEYS];
	bool		spl_risnull[INDEX_MAX_KEYS];
	bool		spl_rightvalid;

	int		   *spl_idgrp;
	int		   *spl_ngrp;		/* number in each group */
	char	   *spl_grpflag;	/* flags of each group */
} GIST_SPLITVEC;

/*
 * An entry on a GiST node.  Contains the key, as well as its own
 * location (rel,page,offset) which can supply the matching pointer.
 * The size of the key is in bytes, and leafkey is a flag to tell us
 * if the entry is in a leaf node.
 */
typedef struct GISTENTRY
{
	Datum		key;
	Relation	rel;
	Page		page;
	OffsetNumber offset;
	int			bytes;
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

#define GEVHDRSZ	(offsetof(GistEntryVector, vector[0]))

/*
 * macro to initialize a GISTENTRY
 */
#define gistentryinit(e, k, r, pg, o, b, l) \
	do { (e).key = (k); (e).rel = (r); (e).page = (pg); \
		 (e).offset = (o); (e).bytes = (b); (e).leafkey = (l); } while (0)

#endif   /* GIST_H */
