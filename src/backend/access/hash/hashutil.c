/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Utility code for Postgres hash implementation.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/hash/hashutil.c,v 1.43 2005/11/06 19:29:00 tgl Exp $
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
	memcpy(&(hitem->hash_itup), itup, tuplen);

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
_hash_checkpage(Relation rel, Buffer buf, int flags)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer verifies that every newly-read page passes PageHeaderIsValid,
	 * which means it either contains a reasonably sane page header or is
	 * all-zero.  We have to defend against the all-zero case, however.
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * Additionally check that the special area looks sane.
	 */
	if (((PageHeader) (page))->pd_special !=
		   (BLCKSZ - MAXALIGN(sizeof(HashPageOpaqueData))))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	if (flags)
	{
		HashPageOpaque opaque = (HashPageOpaque) PageGetSpecialPointer(page);

		if ((opaque->hasho_flag & flags) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" contains corrupted page at block %u",
							RelationGetRelationName(rel),
							BufferGetBlockNumber(buf)),
					 errhint("Please REINDEX it.")));
	}

	/*
	 * When checking the metapage, also verify magic number and version.
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
					 errmsg("index \"%s\" has wrong hash version",
							RelationGetRelationName(rel)),
					 errhint("Please REINDEX it.")));
	}
}
