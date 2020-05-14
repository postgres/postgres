/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Utility code for Postgres hash implementation.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashutil.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "port/pg_bitutils.h"
#include "storage/buf_internals.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#define CALC_NEW_BUCKET(old_bucket, lowmask) \
			old_bucket | (lowmask + 1)

/*
 * _hash_checkqual -- does the index tuple satisfy the scan conditions?
 */
bool
_hash_checkqual(IndexScanDesc scan, IndexTuple itup)
{
	/*
	 * Currently, we can't check any of the scan conditions since we do not
	 * have the original index entry value to supply to the sk_func. Always
	 * return true; we expect that hashgettuple already set the recheck flag
	 * to make the main indexscan code do it.
	 */
#ifdef NOT_USED
	TupleDesc	tupdesc = RelationGetDescr(scan->indexRelation);
	ScanKey		key = scan->keyData;
	int			scanKeySize = scan->numberOfKeys;

	while (scanKeySize > 0)
	{
		Datum		datum;
		bool		isNull;
		Datum		test;

		datum = index_getattr(itup,
							  key->sk_attno,
							  tupdesc,
							  &isNull);

		/* assume sk_func is strict */
		if (isNull)
			return false;
		if (key->sk_flags & SK_ISNULL)
			return false;

		test = FunctionCall2Coll(&key->sk_func, key->sk_collation,
								 datum, key->sk_argument);

		if (!DatumGetBool(test))
			return false;

		key++;
		scanKeySize--;
	}
#endif

	return true;
}

/*
 * _hash_datum2hashkey -- given a Datum, call the index's hash function
 *
 * The Datum is assumed to be of the index's column type, so we can use the
 * "primary" hash function that's tracked for us by the generic index code.
 */
uint32
_hash_datum2hashkey(Relation rel, Datum key)
{
	FmgrInfo   *procinfo;
	Oid			collation;

	/* XXX assumes index has only one attribute */
	procinfo = index_getprocinfo(rel, 1, HASHSTANDARD_PROC);
	collation = rel->rd_indcollation[0];

	return DatumGetUInt32(FunctionCall1Coll(procinfo, collation, key));
}

/*
 * _hash_datum2hashkey_type -- given a Datum of a specified type,
 *			hash it in a fashion compatible with this index
 *
 * This is much more expensive than _hash_datum2hashkey, so use it only in
 * cross-type situations.
 */
uint32
_hash_datum2hashkey_type(Relation rel, Datum key, Oid keytype)
{
	RegProcedure hash_proc;
	Oid			collation;

	/* XXX assumes index has only one attribute */
	hash_proc = get_opfamily_proc(rel->rd_opfamily[0],
								  keytype,
								  keytype,
								  HASHSTANDARD_PROC);
	if (!RegProcedureIsValid(hash_proc))
		elog(ERROR, "missing support function %d(%u,%u) for index \"%s\"",
			 HASHSTANDARD_PROC, keytype, keytype,
			 RelationGetRelationName(rel));
	collation = rel->rd_indcollation[0];

	return DatumGetUInt32(OidFunctionCall1Coll(hash_proc, collation, key));
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
 * _hash_spareindex -- returns spare index / global splitpoint phase of the
 *					   bucket
 */
uint32
_hash_spareindex(uint32 num_bucket)
{
	uint32		splitpoint_group;
	uint32		splitpoint_phases;

	splitpoint_group = pg_ceil_log2_32(num_bucket);

	if (splitpoint_group < HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)
		return splitpoint_group;

	/* account for single-phase groups */
	splitpoint_phases = HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE;

	/* account for multi-phase groups before splitpoint_group */
	splitpoint_phases +=
		((splitpoint_group - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) <<
		 HASH_SPLITPOINT_PHASE_BITS);

	/* account for phases within current group */
	splitpoint_phases +=
		(((num_bucket - 1) >>
		  (splitpoint_group - (HASH_SPLITPOINT_PHASE_BITS + 1))) &
		 HASH_SPLITPOINT_PHASE_MASK);	/* to 0-based value. */

	return splitpoint_phases;
}

/*
 *	_hash_get_totalbuckets -- returns total number of buckets allocated till
 *							the given splitpoint phase.
 */
uint32
_hash_get_totalbuckets(uint32 splitpoint_phase)
{
	uint32		splitpoint_group;
	uint32		total_buckets;
	uint32		phases_within_splitpoint_group;

	if (splitpoint_phase < HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE)
		return (1 << splitpoint_phase);

	/* get splitpoint's group */
	splitpoint_group = HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE;
	splitpoint_group +=
		((splitpoint_phase - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) >>
		 HASH_SPLITPOINT_PHASE_BITS);

	/* account for buckets before splitpoint_group */
	total_buckets = (1 << (splitpoint_group - 1));

	/* account for buckets within splitpoint_group */
	phases_within_splitpoint_group =
		(((splitpoint_phase - HASH_SPLITPOINT_GROUPS_WITH_ONE_PHASE) &
		  HASH_SPLITPOINT_PHASE_MASK) + 1); /* from 0-based to 1-based */
	total_buckets +=
		(((1 << (splitpoint_group - 1)) >> HASH_SPLITPOINT_PHASE_BITS) *
		 phases_within_splitpoint_group);

	return total_buckets;
}

/*
 * _hash_checkpage -- sanity checks on the format of all hash pages
 *
 * If flags is not zero, it is a bitwise OR of the acceptable page types
 * (values of hasho_flag & LH_PAGE_TYPE).
 */
void
_hash_checkpage(Relation rel, Buffer buf, int flags)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer verifies that every newly-read page passes
	 * PageHeaderIsValid, which means it either contains a reasonably sane
	 * page header or is all-zero.  We have to defend against the all-zero
	 * case, however.
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
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(HashPageOpaqueData)))
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
		HashMetaPage metap = HashPageGetMeta(page);

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

bytea *
hashoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(HashOptions, fillfactor)},
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_HASH,
									  sizeof(HashOptions),
									  tab, lengthof(tab));
}

/*
 * _hash_get_indextuple_hashkey - get the hash index tuple's hash key value
 */
uint32
_hash_get_indextuple_hashkey(IndexTuple itup)
{
	char	   *attp;

	/*
	 * We assume the hash key is the first attribute and can't be null, so
	 * this can be done crudely but very very cheaply ...
	 */
	attp = (char *) itup + IndexInfoFindDataOffset(itup->t_info);
	return *((uint32 *) attp);
}

/*
 * _hash_convert_tuple - convert raw index data to hash key
 *
 * Inputs: values and isnull arrays for the user data column(s)
 * Outputs: values and isnull arrays for the index tuple, suitable for
 *		passing to index_form_tuple().
 *
 * Returns true if successful, false if not (because there are null values).
 * On a false result, the given data need not be indexed.
 *
 * Note: callers know that the index-column arrays are always of length 1.
 * In principle, there could be more than one input column, though we do not
 * currently support that.
 */
bool
_hash_convert_tuple(Relation index,
					Datum *user_values, bool *user_isnull,
					Datum *index_values, bool *index_isnull)
{
	uint32		hashkey;

	/*
	 * We do not insert null values into hash indexes.  This is okay because
	 * the only supported search operator is '=', and we assume it is strict.
	 */
	if (user_isnull[0])
		return false;

	hashkey = _hash_datum2hashkey(index, user_values[0]);
	index_values[0] = UInt32GetDatum(hashkey);
	index_isnull[0] = false;
	return true;
}

/*
 * _hash_binsearch - Return the offset number in the page where the
 *					 specified hash value should be sought or inserted.
 *
 * We use binary search, relying on the assumption that the existing entries
 * are ordered by hash key.
 *
 * Returns the offset of the first index entry having hashkey >= hash_value,
 * or the page's max offset plus one if hash_value is greater than all
 * existing hash keys in the page.  This is the appropriate place to start
 * a search, or to insert a new item.
 */
OffsetNumber
_hash_binsearch(Page page, uint32 hash_value)
{
	OffsetNumber upper;
	OffsetNumber lower;

	/* Loop invariant: lower <= desired place <= upper */
	upper = PageGetMaxOffsetNumber(page) + 1;
	lower = FirstOffsetNumber;

	while (upper > lower)
	{
		OffsetNumber off;
		IndexTuple	itup;
		uint32		hashkey;

		off = (upper + lower) / 2;
		Assert(OffsetNumberIsValid(off));

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		hashkey = _hash_get_indextuple_hashkey(itup);
		if (hashkey < hash_value)
			lower = off + 1;
		else
			upper = off;
	}

	return lower;
}

/*
 * _hash_binsearch_last
 *
 * Same as above, except that if there are multiple matching items in the
 * page, we return the offset of the last one instead of the first one,
 * and the possible range of outputs is 0..maxoffset not 1..maxoffset+1.
 * This is handy for starting a new page in a backwards scan.
 */
OffsetNumber
_hash_binsearch_last(Page page, uint32 hash_value)
{
	OffsetNumber upper;
	OffsetNumber lower;

	/* Loop invariant: lower <= desired place <= upper */
	upper = PageGetMaxOffsetNumber(page);
	lower = FirstOffsetNumber - 1;

	while (upper > lower)
	{
		IndexTuple	itup;
		OffsetNumber off;
		uint32		hashkey;

		off = (upper + lower + 1) / 2;
		Assert(OffsetNumberIsValid(off));

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, off));
		hashkey = _hash_get_indextuple_hashkey(itup);
		if (hashkey > hash_value)
			upper = off - 1;
		else
			lower = off;
	}

	return lower;
}

/*
 *	_hash_get_oldblock_from_newbucket() -- get the block number of a bucket
 *			from which current (new) bucket is being split.
 */
BlockNumber
_hash_get_oldblock_from_newbucket(Relation rel, Bucket new_bucket)
{
	Bucket		old_bucket;
	uint32		mask;
	Buffer		metabuf;
	HashMetaPage metap;
	BlockNumber blkno;

	/*
	 * To get the old bucket from the current bucket, we need a mask to modulo
	 * into lower half of table.  This mask is stored in meta page as
	 * hashm_lowmask, but here we can't rely on the same, because we need a
	 * value of lowmask that was prevalent at the time when bucket split was
	 * started.  Masking the most significant bit of new bucket would give us
	 * old bucket.
	 */
	mask = (((uint32) 1) << (fls(new_bucket) - 1)) - 1;
	old_bucket = new_bucket & mask;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	blkno = BUCKET_TO_BLKNO(metap, old_bucket);

	_hash_relbuf(rel, metabuf);

	return blkno;
}

/*
 *	_hash_get_newblock_from_oldbucket() -- get the block number of a bucket
 *			that will be generated after split from old bucket.
 *
 * This is used to find the new bucket from old bucket based on current table
 * half.  It is mainly required to finish the incomplete splits where we are
 * sure that not more than one bucket could have split in progress from old
 * bucket.
 */
BlockNumber
_hash_get_newblock_from_oldbucket(Relation rel, Bucket old_bucket)
{
	Bucket		new_bucket;
	Buffer		metabuf;
	HashMetaPage metap;
	BlockNumber blkno;

	metabuf = _hash_getbuf(rel, HASH_METAPAGE, HASH_READ, LH_META_PAGE);
	metap = HashPageGetMeta(BufferGetPage(metabuf));

	new_bucket = _hash_get_newbucket_from_oldbucket(rel, old_bucket,
													metap->hashm_lowmask,
													metap->hashm_maxbucket);
	blkno = BUCKET_TO_BLKNO(metap, new_bucket);

	_hash_relbuf(rel, metabuf);

	return blkno;
}

/*
 *	_hash_get_newbucket_from_oldbucket() -- get the new bucket that will be
 *			generated after split from current (old) bucket.
 *
 * This is used to find the new bucket from old bucket.  New bucket can be
 * obtained by OR'ing old bucket with most significant bit of current table
 * half (lowmask passed in this function can be used to identify msb of
 * current table half).  There could be multiple buckets that could have
 * been split from current bucket.  We need the first such bucket that exists.
 * Caller must ensure that no more than one split has happened from old
 * bucket.
 */
Bucket
_hash_get_newbucket_from_oldbucket(Relation rel, Bucket old_bucket,
								   uint32 lowmask, uint32 maxbucket)
{
	Bucket		new_bucket;

	new_bucket = CALC_NEW_BUCKET(old_bucket, lowmask);
	if (new_bucket > maxbucket)
	{
		lowmask = lowmask >> 1;
		new_bucket = CALC_NEW_BUCKET(old_bucket, lowmask);
	}

	return new_bucket;
}

/*
 * _hash_kill_items - set LP_DEAD state for items an indexscan caller has
 * told us were killed.
 *
 * scan->opaque, referenced locally through so, contains information about the
 * current page and killed tuples thereon (generally, this should only be
 * called if so->numKilled > 0).
 *
 * The caller does not have a lock on the page and may or may not have the
 * page pinned in a buffer.  Note that read-lock is sufficient for setting
 * LP_DEAD status (which is only a hint).
 *
 * The caller must have pin on bucket buffer, but may or may not have pin
 * on overflow buffer, as indicated by HashScanPosIsPinned(so->currPos).
 *
 * We match items by heap TID before assuming they are the right ones to
 * delete.
 *
 * There are never any scans active in a bucket at the time VACUUM begins,
 * because VACUUM takes a cleanup lock on the primary bucket page and scans
 * hold a pin.  A scan can begin after VACUUM leaves the primary bucket page
 * but before it finishes the entire bucket, but it can never pass VACUUM,
 * because VACUUM always locks the next page before releasing the lock on
 * the previous one.  Therefore, we don't have to worry about accidentally
 * killing a TID that has been reused for an unrelated tuple.
 */
void
_hash_kill_items(IndexScanDesc scan)
{
	HashScanOpaque so = (HashScanOpaque) scan->opaque;
	Relation	rel = scan->indexRelation;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;
	HashPageOpaque opaque;
	OffsetNumber offnum,
				maxoff;
	int			numKilled = so->numKilled;
	int			i;
	bool		killedsomething = false;
	bool		havePin = false;

	Assert(so->numKilled > 0);
	Assert(so->killedItems != NULL);
	Assert(HashScanPosIsValid(so->currPos));

	/*
	 * Always reset the scan state, so we don't look for same items on other
	 * pages.
	 */
	so->numKilled = 0;

	blkno = so->currPos.currPage;
	if (HashScanPosIsPinned(so->currPos))
	{
		/*
		 * We already have pin on this buffer, so, all we need to do is
		 * acquire lock on it.
		 */
		havePin = true;
		buf = so->currPos.buf;
		LockBuffer(buf, BUFFER_LOCK_SHARE);
	}
	else
		buf = _hash_getbuf(rel, blkno, HASH_READ, LH_OVERFLOW_PAGE);

	page = BufferGetPage(buf);
	opaque = (HashPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	for (i = 0; i < numKilled; i++)
	{
		int			itemIndex = so->killedItems[i];
		HashScanPosItem *currItem = &so->currPos.items[itemIndex];

		offnum = currItem->indexOffset;

		Assert(itemIndex >= so->currPos.firstItem &&
			   itemIndex <= so->currPos.lastItem);

		while (offnum <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, offnum);
			IndexTuple	ituple = (IndexTuple) PageGetItem(page, iid);

			if (ItemPointerEquals(&ituple->t_tid, &currItem->heapTid))
			{
				/* found the item */
				ItemIdMarkDead(iid);
				killedsomething = true;
				break;			/* out of inner search loop */
			}
			offnum = OffsetNumberNext(offnum);
		}
	}

	/*
	 * Since this can be redone later if needed, mark as dirty hint. Whenever
	 * we mark anything LP_DEAD, we also set the page's
	 * LH_PAGE_HAS_DEAD_TUPLES flag, which is likewise just a hint.
	 */
	if (killedsomething)
	{
		opaque->hasho_flag |= LH_PAGE_HAS_DEAD_TUPLES;
		MarkBufferDirtyHint(buf, true);
	}

	if (so->hashso_bucket_buf == so->currPos.buf ||
		havePin)
		LockBuffer(so->currPos.buf, BUFFER_LOCK_UNLOCK);
	else
		_hash_relbuf(rel, buf);
}
