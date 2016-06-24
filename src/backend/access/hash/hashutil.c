/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Utility code for Postgres hash implementation.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
#include "utils/lsyscache.h"
#include "utils/rel.h"


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
 * _hash_datum2hashkey -- given a Datum, call the index's hash procedure
 *
 * The Datum is assumed to be of the index's column type, so we can use the
 * "primary" hash procedure that's tracked for us by the generic index code.
 */
uint32
_hash_datum2hashkey(Relation rel, Datum key)
{
	FmgrInfo   *procinfo;
	Oid			collation;

	/* XXX assumes index has only one attribute */
	procinfo = index_getprocinfo(rel, 1, HASHPROC);
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
								  HASHPROC);
	if (!RegProcedureIsValid(hash_proc))
		elog(ERROR, "missing support function %d(%u,%u) for index \"%s\"",
			 HASHPROC, keytype, keytype,
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
 *
 * If flags is not zero, it is a bitwise OR of the acceptable values of
 * hasho_flag.
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

Datum
hashoptions(PG_FUNCTION_ARGS)
{
	Datum		reloptions = PG_GETARG_DATUM(0);
	bool		validate = PG_GETARG_BOOL(1);
	bytea	   *result;

	result = default_reloptions(reloptions, validate, RELOPT_KIND_HASH);

	if (result)
		PG_RETURN_BYTEA_P(result);
	PG_RETURN_NULL();
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
