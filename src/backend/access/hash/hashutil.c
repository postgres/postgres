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
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashutil.c,v 1.35 2003/09/02 18:13:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/iqual.h"


/*
 * _hash_mkscankey -- build a scan key matching the given indextuple
 *
 * Note: this is prepared for multiple index columns, but very little
 * else in access/hash is ...
 */
ScanKey
_hash_mkscankey(Relation rel, IndexTuple itup)
{
	ScanKey		skey;
	TupleDesc	itupdesc = RelationGetDescr(rel);
	int			natts = rel->rd_rel->relnatts;
	AttrNumber	i;
	Datum		arg;
	FmgrInfo   *procinfo;
	bool		isnull;

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		arg = index_getattr(itup, i + 1, itupdesc, &isnull);
		procinfo = index_getprocinfo(rel, i + 1, HASHPROC);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   isnull ? SK_ISNULL : 0x0,
									   (AttrNumber) (i + 1),
									   procinfo,
									   CurrentMemoryContext,
									   arg);
	}

	return skey;
}

void
_hash_freeskey(ScanKey skey)
{
	pfree(skey);
}

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
				 errmsg("hash indexes cannot include null keys")));

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
 * _hash_call -- given a Datum, call the index's hash procedure
 *
 * Returns the bucket number that the hash key maps to.
 */
Bucket
_hash_call(Relation rel, HashMetaPage metap, Datum key)
{
	FmgrInfo   *procinfo;
	uint32		n;
	Bucket		bucket;

	/* XXX assumes index has only one attribute */
	procinfo = index_getprocinfo(rel, 1, HASHPROC);
	n = DatumGetUInt32(FunctionCall1(procinfo, key));

	bucket = n & metap->hashm_highmask;
	if (bucket > metap->hashm_maxbucket)
		bucket = bucket & metap->hashm_lowmask;

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
					 errmsg("index \"%s\" has wrong hash version, please REINDEX it",
							RelationGetRelationName(rel))));
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
