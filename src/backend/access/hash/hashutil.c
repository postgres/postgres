/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Utility code for Postgres hash implementation.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashutil.c,v 1.37 2003/09/25 06:57:56 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/iqual.h"


/*
 * _hash_checkqual -- does the index tuple satisfy the scan conditions?
 */
bool
_hash_checkqual(IndexScanDesc scan, IndexTuple itup)
{
	return index_keytest(itup, RelationGetDescr(scan->indexRelation),
						 scan->numberOfKeys, scan->keyData);
}

/*
 * _hash_formitem -- construct a hash index entry
 */
HashItem
_hash_formitem(IndexTuple itup)
{
	int			nbytes_hitem;
	HashItem	hitem;
	Size		tuplen;

	/* disallow nulls in hash keys */
	if (IndexTupleHasNulls(itup))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("hash indexes cannot contain null keys")));

	/*
	 * make a copy of the index tuple (XXX do we still need to copy?)
	 *
	 * HashItemData used to have more fields than IndexTupleData, but no
	 * longer...
	 */
	tuplen = IndexTupleSize(itup);
	nbytes_hitem = tuplen +
		(sizeof(HashItemData) - sizeof(IndexTupleData));

	hitem = (HashItem) palloc(nbytes_hitem);
	memcpy((char *) &(hitem->hash_itup), (char *) itup, tuplen);

	return hitem;
}

/*
 * _hash_datum2hashkey -- given a Datum, call the index's hash procedure
 */
uint32
_hash_datum2hashkey(Relation rel, Datum key)
{
	FmgrInfo   *procinfo;

	/* XXX assumes index has only one attribute */
	procinfo = index_getprocinfo(rel, 1, HASHPROC);

	return DatumGetUInt32(FunctionCall1(procinfo, key));
}

/*
 * _hash_hashkey2bucket -- determine which bucket the hashkey maps to.
 */
Bucket
_hash_hashkey2bucket(uint32 hashkey, uint32 maxbucket,
					 uint32 highmask, uint32 lowmask)
{
	Bucket		bucket;

	bucket = hashkey & highmask;
	if (bucket > maxbucket)
		bucket = bucket & lowmask;

	return bucket;
}

/*
 * _hash_log2 -- returns ceil(lg2(num))
 */
uint32
_hash_log2(uint32 num)
{
	uint32		i,
				limit;

	limit = 1;
	for (i = 0; limit < num; limit <<= 1, i++)
		;
	return i;
}

/*
 * _hash_checkpage -- sanity checks on the format of all hash pages
 */
void
_hash_checkpage(Relation rel, Page page, int flags)
{
	Assert(page);
	/*
	 * When checking the metapage, always verify magic number and version.
	 */
	if (flags == LH_META_PAGE)
	{
		HashMetaPage metap = (HashMetaPage) page;

		if (metap->hashm_magic != HASH_MAGIC)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" is not a hash index",
							RelationGetRelationName(rel))));

		if (metap->hashm_version != HASH_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" has wrong hash version", RelationGetRelationName(rel)),
					 errhint("Please REINDEX it.")));
	}

	/*
	 * These other checks are for debugging purposes only.
	 */
#ifdef USE_ASSERT_CHECKING
	Assert(((PageHeader) (page))->pd_lower >= SizeOfPageHeaderData);
	Assert(((PageHeader) (page))->pd_upper <=
		   (BLCKSZ - MAXALIGN(sizeof(HashPageOpaqueData))));
	Assert(((PageHeader) (page))->pd_special ==
		   (BLCKSZ - MAXALIGN(sizeof(HashPageOpaqueData))));
	Assert(PageGetPageSize(page) == BLCKSZ);
	if (flags)
	{
		HashPageOpaque opaque = (HashPageOpaque) PageGetSpecialPointer(page);

		Assert(opaque->hasho_flag & flags);
	}
#endif   /* USE_ASSERT_CHECKING */
}
