/*-------------------------------------------------------------------------
 *
 * hashutil.c
 *	  Utility code for Postgres hash implementation.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashutil.c,v 1.32 2003/07/21 20:29:38 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/iqual.h"


ScanKey
_hash_mkscankey(Relation rel, IndexTuple itup)
{
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			natts;
	AttrNumber	i;
	Datum		arg;
	FmgrInfo   *procinfo;
	bool		isnull;

	natts = rel->rd_rel->relnatts;
	itupdesc = RelationGetDescr(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		arg = index_getattr(itup, i + 1, itupdesc, &isnull);
		procinfo = index_getprocinfo(rel, i + 1, HASHPROC);
		ScanKeyEntryInitializeWithInfo(&skey[i],
									   0x0,
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


bool
_hash_checkqual(IndexScanDesc scan, IndexTuple itup)
{
	if (scan->numberOfKeys > 0)
		return (index_keytest(itup,
							  RelationGetDescr(scan->indexRelation),
							  scan->numberOfKeys, scan->keyData));
	else
		return true;
}

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

	/* make a copy of the index tuple with room for the sequence number */
	tuplen = IndexTupleSize(itup);
	nbytes_hitem = tuplen +
		(sizeof(HashItemData) - sizeof(IndexTupleData));

	hitem = (HashItem) palloc(nbytes_hitem);
	memmove((char *) &(hitem->hash_itup), (char *) itup, tuplen);

	return hitem;
}

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
	for (i = 0; limit < num; limit = limit << 1, i++)
		;
	return i;
}

/*
 * _hash_checkpage -- sanity checks on the format of all hash pages
 */
void
_hash_checkpage(Page page, int flags)
{
	HashPageOpaque opaque;

	Assert(page);
	Assert(((PageHeader) (page))->pd_lower >= SizeOfPageHeaderData);
#if 1
	Assert(((PageHeader) (page))->pd_upper <=
		   (BLCKSZ - MAXALIGN(sizeof(HashPageOpaqueData))));
	Assert(((PageHeader) (page))->pd_special ==
		   (BLCKSZ - MAXALIGN(sizeof(HashPageOpaqueData))));
	Assert(PageGetPageSize(page) == BLCKSZ);
#endif
	if (flags)
	{
		opaque = (HashPageOpaque) PageGetSpecialPointer(page);
		Assert(opaque->hasho_flag & flags);
	}
}
