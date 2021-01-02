/*-------------------------------------------------------------------------
 *
 * hash_xlog.c
 *	  WAL replay logic for hash index.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hash_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/hash.h"
#include "access/hash_xlog.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/procarray.h"

/*
 * replay a hash index meta page
 */
static void
hash_xlog_init_meta_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Page		page;
	Buffer		metabuf;
	ForkNumber	forknum;

	xl_hash_init_meta_page *xlrec = (xl_hash_init_meta_page *) XLogRecGetData(record);

	/* create the index' metapage */
	metabuf = XLogInitBufferForRedo(record, 0);
	Assert(BufferIsValid(metabuf));
	_hash_init_metabuffer(metabuf, xlrec->num_tuples, xlrec->procid,
						  xlrec->ffactor, true);
	page = (Page) BufferGetPage(metabuf);
	PageSetLSN(page, lsn);
	MarkBufferDirty(metabuf);

	/*
	 * Force the on-disk state of init forks to always be in sync with the
	 * state in shared buffers.  See XLogReadBufferForRedoExtended.  We need
	 * special handling for init forks as create index operations don't log a
	 * full page image of the metapage.
	 */
	XLogRecGetBlockTag(record, 0, NULL, &forknum, NULL);
	if (forknum == INIT_FORKNUM)
		FlushOneBuffer(metabuf);

	/* all done */
	UnlockReleaseBuffer(metabuf);
}

/*
 * replay a hash index bitmap page
 */
static void
hash_xlog_init_bitmap_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		bitmapbuf;
	Buffer		metabuf;
	Page		page;
	HashMetaPage metap;
	uint32		num_buckets;
	ForkNumber	forknum;

	xl_hash_init_bitmap_page *xlrec = (xl_hash_init_bitmap_page *) XLogRecGetData(record);

	/*
	 * Initialize bitmap page
	 */
	bitmapbuf = XLogInitBufferForRedo(record, 0);
	_hash_initbitmapbuffer(bitmapbuf, xlrec->bmsize, true);
	PageSetLSN(BufferGetPage(bitmapbuf), lsn);
	MarkBufferDirty(bitmapbuf);

	/*
	 * Force the on-disk state of init forks to always be in sync with the
	 * state in shared buffers.  See XLogReadBufferForRedoExtended.  We need
	 * special handling for init forks as create index operations don't log a
	 * full page image of the metapage.
	 */
	XLogRecGetBlockTag(record, 0, NULL, &forknum, NULL);
	if (forknum == INIT_FORKNUM)
		FlushOneBuffer(bitmapbuf);
	UnlockReleaseBuffer(bitmapbuf);

	/* add the new bitmap page to the metapage's list of bitmaps */
	if (XLogReadBufferForRedo(record, 1, &metabuf) == BLK_NEEDS_REDO)
	{
		/*
		 * Note: in normal operation, we'd update the metapage while still
		 * holding lock on the bitmap page.  But during replay it's not
		 * necessary to hold that lock, since nobody can see it yet; the
		 * creating transaction hasn't yet committed.
		 */
		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);

		num_buckets = metap->hashm_maxbucket + 1;
		metap->hashm_mapp[metap->hashm_nmaps] = num_buckets + 1;
		metap->hashm_nmaps++;

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);

		XLogRecGetBlockTag(record, 1, NULL, &forknum, NULL);
		if (forknum == INIT_FORKNUM)
			FlushOneBuffer(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * replay a hash index insert without split
 */
static void
hash_xlog_insert(XLogReaderState *record)
{
	HashMetaPage metap;
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_insert *xlrec = (xl_hash_insert *) XLogRecGetData(record);
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		Size		datalen;
		char	   *datapos = XLogRecGetBlockData(record, 0, &datalen);

		page = BufferGetPage(buffer);

		if (PageAddItem(page, (Item) datapos, datalen, xlrec->offnum,
						false, false) == InvalidOffsetNumber)
			elog(PANIC, "hash_xlog_insert: failed to add item");

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	if (XLogReadBufferForRedo(record, 1, &buffer) == BLK_NEEDS_REDO)
	{
		/*
		 * Note: in normal operation, we'd update the metapage while still
		 * holding lock on the page we inserted into.  But during replay it's
		 * not necessary to hold that lock, since no other index updates can
		 * be happening concurrently.
		 */
		page = BufferGetPage(buffer);
		metap = HashPageGetMeta(page);
		metap->hashm_ntuples += 1;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * replay addition of overflow page for hash index
 */
static void
hash_xlog_add_ovfl_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_add_ovfl_page *xlrec = (xl_hash_add_ovfl_page *) XLogRecGetData(record);
	Buffer		leftbuf;
	Buffer		ovflbuf;
	Buffer		metabuf;
	BlockNumber leftblk;
	BlockNumber rightblk;
	BlockNumber newmapblk = InvalidBlockNumber;
	Page		ovflpage;
	HashPageOpaque ovflopaque;
	uint32	   *num_bucket;
	char	   *data;
	Size		datalen PG_USED_FOR_ASSERTS_ONLY;
	bool		new_bmpage = false;

	XLogRecGetBlockTag(record, 0, NULL, NULL, &rightblk);
	XLogRecGetBlockTag(record, 1, NULL, NULL, &leftblk);

	ovflbuf = XLogInitBufferForRedo(record, 0);
	Assert(BufferIsValid(ovflbuf));

	data = XLogRecGetBlockData(record, 0, &datalen);
	num_bucket = (uint32 *) data;
	Assert(datalen == sizeof(uint32));
	_hash_initbuf(ovflbuf, InvalidBlockNumber, *num_bucket, LH_OVERFLOW_PAGE,
				  true);
	/* update backlink */
	ovflpage = BufferGetPage(ovflbuf);
	ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);
	ovflopaque->hasho_prevblkno = leftblk;

	PageSetLSN(ovflpage, lsn);
	MarkBufferDirty(ovflbuf);

	if (XLogReadBufferForRedo(record, 1, &leftbuf) == BLK_NEEDS_REDO)
	{
		Page		leftpage;
		HashPageOpaque leftopaque;

		leftpage = BufferGetPage(leftbuf);
		leftopaque = (HashPageOpaque) PageGetSpecialPointer(leftpage);
		leftopaque->hasho_nextblkno = rightblk;

		PageSetLSN(leftpage, lsn);
		MarkBufferDirty(leftbuf);
	}

	if (BufferIsValid(leftbuf))
		UnlockReleaseBuffer(leftbuf);
	UnlockReleaseBuffer(ovflbuf);

	/*
	 * Note: in normal operation, we'd update the bitmap and meta page while
	 * still holding lock on the overflow pages.  But during replay it's not
	 * necessary to hold those locks, since no other index updates can be
	 * happening concurrently.
	 */
	if (XLogRecHasBlockRef(record, 2))
	{
		Buffer		mapbuffer;

		if (XLogReadBufferForRedo(record, 2, &mapbuffer) == BLK_NEEDS_REDO)
		{
			Page		mappage = (Page) BufferGetPage(mapbuffer);
			uint32	   *freep = NULL;
			char	   *data;
			uint32	   *bitmap_page_bit;

			freep = HashPageGetBitmap(mappage);

			data = XLogRecGetBlockData(record, 2, &datalen);
			bitmap_page_bit = (uint32 *) data;

			SETBIT(freep, *bitmap_page_bit);

			PageSetLSN(mappage, lsn);
			MarkBufferDirty(mapbuffer);
		}
		if (BufferIsValid(mapbuffer))
			UnlockReleaseBuffer(mapbuffer);
	}

	if (XLogRecHasBlockRef(record, 3))
	{
		Buffer		newmapbuf;

		newmapbuf = XLogInitBufferForRedo(record, 3);

		_hash_initbitmapbuffer(newmapbuf, xlrec->bmsize, true);

		new_bmpage = true;
		newmapblk = BufferGetBlockNumber(newmapbuf);

		MarkBufferDirty(newmapbuf);
		PageSetLSN(BufferGetPage(newmapbuf), lsn);

		UnlockReleaseBuffer(newmapbuf);
	}

	if (XLogReadBufferForRedo(record, 4, &metabuf) == BLK_NEEDS_REDO)
	{
		HashMetaPage metap;
		Page		page;
		uint32	   *firstfree_ovflpage;

		data = XLogRecGetBlockData(record, 4, &datalen);
		firstfree_ovflpage = (uint32 *) data;

		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);
		metap->hashm_firstfree = *firstfree_ovflpage;

		if (!xlrec->bmpage_found)
		{
			metap->hashm_spares[metap->hashm_ovflpoint]++;

			if (new_bmpage)
			{
				Assert(BlockNumberIsValid(newmapblk));

				metap->hashm_mapp[metap->hashm_nmaps] = newmapblk;
				metap->hashm_nmaps++;
				metap->hashm_spares[metap->hashm_ovflpoint]++;
			}
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * replay allocation of page for split operation
 */
static void
hash_xlog_split_allocate_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_split_allocate_page *xlrec = (xl_hash_split_allocate_page *) XLogRecGetData(record);
	Buffer		oldbuf;
	Buffer		newbuf;
	Buffer		metabuf;
	Size		datalen PG_USED_FOR_ASSERTS_ONLY;
	char	   *data;
	XLogRedoAction action;

	/*
	 * To be consistent with normal operation, here we take cleanup locks on
	 * both the old and new buckets even though there can't be any concurrent
	 * inserts.
	 */

	/* replay the record for old bucket */
	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &oldbuf);

	/*
	 * Note that we still update the page even if it was restored from a full
	 * page image, because the special space is not included in the image.
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		oldpage;
		HashPageOpaque oldopaque;

		oldpage = BufferGetPage(oldbuf);
		oldopaque = (HashPageOpaque) PageGetSpecialPointer(oldpage);

		oldopaque->hasho_flag = xlrec->old_bucket_flag;
		oldopaque->hasho_prevblkno = xlrec->new_bucket;

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuf);
	}

	/* replay the record for new bucket */
	newbuf = XLogInitBufferForRedo(record, 1);
	_hash_initbuf(newbuf, xlrec->new_bucket, xlrec->new_bucket,
				  xlrec->new_bucket_flag, true);
	if (!IsBufferCleanupOK(newbuf))
		elog(PANIC, "hash_xlog_split_allocate_page: failed to acquire cleanup lock");
	MarkBufferDirty(newbuf);
	PageSetLSN(BufferGetPage(newbuf), lsn);

	/*
	 * We can release the lock on old bucket early as well but doing here to
	 * consistent with normal operation.
	 */
	if (BufferIsValid(oldbuf))
		UnlockReleaseBuffer(oldbuf);
	if (BufferIsValid(newbuf))
		UnlockReleaseBuffer(newbuf);

	/*
	 * Note: in normal operation, we'd update the meta page while still
	 * holding lock on the old and new bucket pages.  But during replay it's
	 * not necessary to hold those locks, since no other bucket splits can be
	 * happening concurrently.
	 */

	/* replay the record for metapage changes */
	if (XLogReadBufferForRedo(record, 2, &metabuf) == BLK_NEEDS_REDO)
	{
		Page		page;
		HashMetaPage metap;

		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);
		metap->hashm_maxbucket = xlrec->new_bucket;

		data = XLogRecGetBlockData(record, 2, &datalen);

		if (xlrec->flags & XLH_SPLIT_META_UPDATE_MASKS)
		{
			uint32		lowmask;
			uint32	   *highmask;

			/* extract low and high masks. */
			memcpy(&lowmask, data, sizeof(uint32));
			highmask = (uint32 *) ((char *) data + sizeof(uint32));

			/* update metapage */
			metap->hashm_lowmask = lowmask;
			metap->hashm_highmask = *highmask;

			data += sizeof(uint32) * 2;
		}

		if (xlrec->flags & XLH_SPLIT_META_UPDATE_SPLITPOINT)
		{
			uint32		ovflpoint;
			uint32	   *ovflpages;

			/* extract information of overflow pages. */
			memcpy(&ovflpoint, data, sizeof(uint32));
			ovflpages = (uint32 *) ((char *) data + sizeof(uint32));

			/* update metapage */
			metap->hashm_spares[ovflpoint] = *ovflpages;
			metap->hashm_ovflpoint = ovflpoint;
		}

		MarkBufferDirty(metabuf);
		PageSetLSN(BufferGetPage(metabuf), lsn);
	}

	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * replay of split operation
 */
static void
hash_xlog_split_page(XLogReaderState *record)
{
	Buffer		buf;

	if (XLogReadBufferForRedo(record, 0, &buf) != BLK_RESTORED)
		elog(ERROR, "Hash split record did not contain a full-page image");

	UnlockReleaseBuffer(buf);
}

/*
 * replay completion of split operation
 */
static void
hash_xlog_split_complete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_split_complete *xlrec = (xl_hash_split_complete *) XLogRecGetData(record);
	Buffer		oldbuf;
	Buffer		newbuf;
	XLogRedoAction action;

	/* replay the record for old bucket */
	action = XLogReadBufferForRedo(record, 0, &oldbuf);

	/*
	 * Note that we still update the page even if it was restored from a full
	 * page image, because the bucket flag is not included in the image.
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		oldpage;
		HashPageOpaque oldopaque;

		oldpage = BufferGetPage(oldbuf);
		oldopaque = (HashPageOpaque) PageGetSpecialPointer(oldpage);

		oldopaque->hasho_flag = xlrec->old_bucket_flag;

		PageSetLSN(oldpage, lsn);
		MarkBufferDirty(oldbuf);
	}
	if (BufferIsValid(oldbuf))
		UnlockReleaseBuffer(oldbuf);

	/* replay the record for new bucket */
	action = XLogReadBufferForRedo(record, 1, &newbuf);

	/*
	 * Note that we still update the page even if it was restored from a full
	 * page image, because the bucket flag is not included in the image.
	 */
	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		Page		newpage;
		HashPageOpaque nopaque;

		newpage = BufferGetPage(newbuf);
		nopaque = (HashPageOpaque) PageGetSpecialPointer(newpage);

		nopaque->hasho_flag = xlrec->new_bucket_flag;

		PageSetLSN(newpage, lsn);
		MarkBufferDirty(newbuf);
	}
	if (BufferIsValid(newbuf))
		UnlockReleaseBuffer(newbuf);
}

/*
 * replay move of page contents for squeeze operation of hash index
 */
static void
hash_xlog_move_page_contents(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_move_page_contents *xldata = (xl_hash_move_page_contents *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		writebuf = InvalidBuffer;
	Buffer		deletebuf = InvalidBuffer;
	XLogRedoAction action;

	/*
	 * Ensure we have a cleanup lock on primary bucket page before we start
	 * with the actual replay operation.  This is to ensure that neither a
	 * scan can start nor a scan can be already-in-progress during the replay
	 * of this operation.  If we allow scans during this operation, then they
	 * can miss some records or show the same record multiple times.
	 */
	if (xldata->is_prim_bucket_same_wrt)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &writebuf);
	else
	{
		/*
		 * we don't care for return value as the purpose of reading bucketbuf
		 * is to ensure a cleanup lock on primary bucket page.
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &writebuf);
	}

	/* replay the record for adding entries in overflow buffer */
	if (action == BLK_NEEDS_REDO)
	{
		Page		writepage;
		char	   *begin;
		char	   *data;
		Size		datalen;
		uint16		ninserted = 0;

		data = begin = XLogRecGetBlockData(record, 1, &datalen);

		writepage = (Page) BufferGetPage(writebuf);

		if (xldata->ntups > 0)
		{
			OffsetNumber *towrite = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntups;

			while (data - begin < datalen)
			{
				IndexTuple	itup = (IndexTuple) data;
				Size		itemsz;
				OffsetNumber l;

				itemsz = IndexTupleSize(itup);
				itemsz = MAXALIGN(itemsz);

				data += itemsz;

				l = PageAddItem(writepage, (Item) itup, itemsz, towrite[ninserted], false, false);
				if (l == InvalidOffsetNumber)
					elog(ERROR, "hash_xlog_move_page_contents: failed to add item to hash index page, size %d bytes",
						 (int) itemsz);

				ninserted++;
			}
		}

		/*
		 * number of tuples inserted must be same as requested in REDO record.
		 */
		Assert(ninserted == xldata->ntups);

		PageSetLSN(writepage, lsn);
		MarkBufferDirty(writebuf);
	}

	/* replay the record for deleting entries from overflow buffer */
	if (XLogReadBufferForRedo(record, 2, &deletebuf) == BLK_NEEDS_REDO)
	{
		Page		page;
		char	   *ptr;
		Size		len;

		ptr = XLogRecGetBlockData(record, 2, &len);

		page = (Page) BufferGetPage(deletebuf);

		if (len > 0)
		{
			OffsetNumber *unused;
			OffsetNumber *unend;

			unused = (OffsetNumber *) ptr;
			unend = (OffsetNumber *) ((char *) ptr + len);

			if ((unend - unused) > 0)
				PageIndexMultiDelete(page, unused, unend - unused);
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(deletebuf);
	}

	/*
	 * Replay is complete, now we can release the buffers. We release locks at
	 * end of replay operation to ensure that we hold lock on primary bucket
	 * page till end of operation.  We can optimize by releasing the lock on
	 * write buffer as soon as the operation for same is complete, if it is
	 * not same as primary bucket page, but that doesn't seem to be worth
	 * complicating the code.
	 */
	if (BufferIsValid(deletebuf))
		UnlockReleaseBuffer(deletebuf);

	if (BufferIsValid(writebuf))
		UnlockReleaseBuffer(writebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);
}

/*
 * replay squeeze page operation of hash index
 */
static void
hash_xlog_squeeze_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_squeeze_page *xldata = (xl_hash_squeeze_page *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		writebuf;
	Buffer		ovflbuf;
	Buffer		prevbuf = InvalidBuffer;
	Buffer		mapbuf;
	XLogRedoAction action;

	/*
	 * Ensure we have a cleanup lock on primary bucket page before we start
	 * with the actual replay operation.  This is to ensure that neither a
	 * scan can start nor a scan can be already-in-progress during the replay
	 * of this operation.  If we allow scans during this operation, then they
	 * can miss some records or show the same record multiple times.
	 */
	if (xldata->is_prim_bucket_same_wrt)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &writebuf);
	else
	{
		/*
		 * we don't care for return value as the purpose of reading bucketbuf
		 * is to ensure a cleanup lock on primary bucket page.
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &writebuf);
	}

	/* replay the record for adding entries in overflow buffer */
	if (action == BLK_NEEDS_REDO)
	{
		Page		writepage;
		char	   *begin;
		char	   *data;
		Size		datalen;
		uint16		ninserted = 0;

		data = begin = XLogRecGetBlockData(record, 1, &datalen);

		writepage = (Page) BufferGetPage(writebuf);

		if (xldata->ntups > 0)
		{
			OffsetNumber *towrite = (OffsetNumber *) data;

			data += sizeof(OffsetNumber) * xldata->ntups;

			while (data - begin < datalen)
			{
				IndexTuple	itup = (IndexTuple) data;
				Size		itemsz;
				OffsetNumber l;

				itemsz = IndexTupleSize(itup);
				itemsz = MAXALIGN(itemsz);

				data += itemsz;

				l = PageAddItem(writepage, (Item) itup, itemsz, towrite[ninserted], false, false);
				if (l == InvalidOffsetNumber)
					elog(ERROR, "hash_xlog_squeeze_page: failed to add item to hash index page, size %d bytes",
						 (int) itemsz);

				ninserted++;
			}
		}

		/*
		 * number of tuples inserted must be same as requested in REDO record.
		 */
		Assert(ninserted == xldata->ntups);

		/*
		 * if the page on which are adding tuples is a page previous to freed
		 * overflow page, then update its nextblkno.
		 */
		if (xldata->is_prev_bucket_same_wrt)
		{
			HashPageOpaque writeopaque = (HashPageOpaque) PageGetSpecialPointer(writepage);

			writeopaque->hasho_nextblkno = xldata->nextblkno;
		}

		PageSetLSN(writepage, lsn);
		MarkBufferDirty(writebuf);
	}

	/* replay the record for initializing overflow buffer */
	if (XLogReadBufferForRedo(record, 2, &ovflbuf) == BLK_NEEDS_REDO)
	{
		Page		ovflpage;
		HashPageOpaque ovflopaque;

		ovflpage = BufferGetPage(ovflbuf);

		_hash_pageinit(ovflpage, BufferGetPageSize(ovflbuf));

		ovflopaque = (HashPageOpaque) PageGetSpecialPointer(ovflpage);

		ovflopaque->hasho_prevblkno = InvalidBlockNumber;
		ovflopaque->hasho_nextblkno = InvalidBlockNumber;
		ovflopaque->hasho_bucket = -1;
		ovflopaque->hasho_flag = LH_UNUSED_PAGE;
		ovflopaque->hasho_page_id = HASHO_PAGE_ID;

		PageSetLSN(ovflpage, lsn);
		MarkBufferDirty(ovflbuf);
	}
	if (BufferIsValid(ovflbuf))
		UnlockReleaseBuffer(ovflbuf);

	/* replay the record for page previous to the freed overflow page */
	if (!xldata->is_prev_bucket_same_wrt &&
		XLogReadBufferForRedo(record, 3, &prevbuf) == BLK_NEEDS_REDO)
	{
		Page		prevpage = BufferGetPage(prevbuf);
		HashPageOpaque prevopaque = (HashPageOpaque) PageGetSpecialPointer(prevpage);

		prevopaque->hasho_nextblkno = xldata->nextblkno;

		PageSetLSN(prevpage, lsn);
		MarkBufferDirty(prevbuf);
	}
	if (BufferIsValid(prevbuf))
		UnlockReleaseBuffer(prevbuf);

	/* replay the record for page next to the freed overflow page */
	if (XLogRecHasBlockRef(record, 4))
	{
		Buffer		nextbuf;

		if (XLogReadBufferForRedo(record, 4, &nextbuf) == BLK_NEEDS_REDO)
		{
			Page		nextpage = BufferGetPage(nextbuf);
			HashPageOpaque nextopaque = (HashPageOpaque) PageGetSpecialPointer(nextpage);

			nextopaque->hasho_prevblkno = xldata->prevblkno;

			PageSetLSN(nextpage, lsn);
			MarkBufferDirty(nextbuf);
		}
		if (BufferIsValid(nextbuf))
			UnlockReleaseBuffer(nextbuf);
	}

	if (BufferIsValid(writebuf))
		UnlockReleaseBuffer(writebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);

	/*
	 * Note: in normal operation, we'd update the bitmap and meta page while
	 * still holding lock on the primary bucket page and overflow pages.  But
	 * during replay it's not necessary to hold those locks, since no other
	 * index updates can be happening concurrently.
	 */
	/* replay the record for bitmap page */
	if (XLogReadBufferForRedo(record, 5, &mapbuf) == BLK_NEEDS_REDO)
	{
		Page		mappage = (Page) BufferGetPage(mapbuf);
		uint32	   *freep = NULL;
		char	   *data;
		uint32	   *bitmap_page_bit;
		Size		datalen;

		freep = HashPageGetBitmap(mappage);

		data = XLogRecGetBlockData(record, 5, &datalen);
		bitmap_page_bit = (uint32 *) data;

		CLRBIT(freep, *bitmap_page_bit);

		PageSetLSN(mappage, lsn);
		MarkBufferDirty(mapbuf);
	}
	if (BufferIsValid(mapbuf))
		UnlockReleaseBuffer(mapbuf);

	/* replay the record for meta page */
	if (XLogRecHasBlockRef(record, 6))
	{
		Buffer		metabuf;

		if (XLogReadBufferForRedo(record, 6, &metabuf) == BLK_NEEDS_REDO)
		{
			HashMetaPage metap;
			Page		page;
			char	   *data;
			uint32	   *firstfree_ovflpage;
			Size		datalen;

			data = XLogRecGetBlockData(record, 6, &datalen);
			firstfree_ovflpage = (uint32 *) data;

			page = BufferGetPage(metabuf);
			metap = HashPageGetMeta(page);
			metap->hashm_firstfree = *firstfree_ovflpage;

			PageSetLSN(page, lsn);
			MarkBufferDirty(metabuf);
		}
		if (BufferIsValid(metabuf))
			UnlockReleaseBuffer(metabuf);
	}
}

/*
 * replay delete operation of hash index
 */
static void
hash_xlog_delete(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_delete *xldata = (xl_hash_delete *) XLogRecGetData(record);
	Buffer		bucketbuf = InvalidBuffer;
	Buffer		deletebuf;
	Page		page;
	XLogRedoAction action;

	/*
	 * Ensure we have a cleanup lock on primary bucket page before we start
	 * with the actual replay operation.  This is to ensure that neither a
	 * scan can start nor a scan can be already-in-progress during the replay
	 * of this operation.  If we allow scans during this operation, then they
	 * can miss some records or show the same record multiple times.
	 */
	if (xldata->is_primary_bucket_page)
		action = XLogReadBufferForRedoExtended(record, 1, RBM_NORMAL, true, &deletebuf);
	else
	{
		/*
		 * we don't care for return value as the purpose of reading bucketbuf
		 * is to ensure a cleanup lock on primary bucket page.
		 */
		(void) XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &bucketbuf);

		action = XLogReadBufferForRedo(record, 1, &deletebuf);
	}

	/* replay the record for deleting entries in bucket page */
	if (action == BLK_NEEDS_REDO)
	{
		char	   *ptr;
		Size		len;

		ptr = XLogRecGetBlockData(record, 1, &len);

		page = (Page) BufferGetPage(deletebuf);

		if (len > 0)
		{
			OffsetNumber *unused;
			OffsetNumber *unend;

			unused = (OffsetNumber *) ptr;
			unend = (OffsetNumber *) ((char *) ptr + len);

			if ((unend - unused) > 0)
				PageIndexMultiDelete(page, unused, unend - unused);
		}

		/*
		 * Mark the page as not containing any LP_DEAD items only if
		 * clear_dead_marking flag is set to true. See comments in
		 * hashbucketcleanup() for details.
		 */
		if (xldata->clear_dead_marking)
		{
			HashPageOpaque pageopaque;

			pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
			pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
		}

		PageSetLSN(page, lsn);
		MarkBufferDirty(deletebuf);
	}
	if (BufferIsValid(deletebuf))
		UnlockReleaseBuffer(deletebuf);

	if (BufferIsValid(bucketbuf))
		UnlockReleaseBuffer(bucketbuf);
}

/*
 * replay split cleanup flag operation for primary bucket page.
 */
static void
hash_xlog_split_cleanup(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffer;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &buffer) == BLK_NEEDS_REDO)
	{
		HashPageOpaque bucket_opaque;

		page = (Page) BufferGetPage(buffer);

		bucket_opaque = (HashPageOpaque) PageGetSpecialPointer(page);
		bucket_opaque->hasho_flag &= ~LH_BUCKET_NEEDS_SPLIT_CLEANUP;
		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
}

/*
 * replay for update meta page
 */
static void
hash_xlog_update_meta_page(XLogReaderState *record)
{
	HashMetaPage metap;
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_update_meta_page *xldata = (xl_hash_update_meta_page *) XLogRecGetData(record);
	Buffer		metabuf;
	Page		page;

	if (XLogReadBufferForRedo(record, 0, &metabuf) == BLK_NEEDS_REDO)
	{
		page = BufferGetPage(metabuf);
		metap = HashPageGetMeta(page);

		metap->hashm_ntuples = xldata->ntuples;

		PageSetLSN(page, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

/*
 * replay delete operation in hash index to remove
 * tuples marked as DEAD during index tuple insertion.
 */
static void
hash_xlog_vacuum_one_page(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	xl_hash_vacuum_one_page *xldata;
	Buffer		buffer;
	Buffer		metabuf;
	Page		page;
	XLogRedoAction action;
	HashPageOpaque pageopaque;

	xldata = (xl_hash_vacuum_one_page *) XLogRecGetData(record);

	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page.
	 *
	 * Hash index records that are marked as LP_DEAD and being removed during
	 * hash index tuple insertion can conflict with standby queries. You might
	 * think that vacuum records would conflict as well, but we've handled
	 * that already.  XLOG_HEAP2_CLEANUP_INFO records provide the highest xid
	 * cleaned by the vacuum of the heap and so we can resolve any conflicts
	 * just once when that arrives.  After that we know that no conflicts
	 * exist from individual hash index vacuum records on that index.
	 */
	if (InHotStandby)
	{
		RelFileNode rnode;

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);
		ResolveRecoveryConflictWithSnapshot(xldata->latestRemovedXid, rnode);
	}

	action = XLogReadBufferForRedoExtended(record, 0, RBM_NORMAL, true, &buffer);

	if (action == BLK_NEEDS_REDO)
	{
		page = (Page) BufferGetPage(buffer);

		if (XLogRecGetDataLen(record) > SizeOfHashVacuumOnePage)
		{
			OffsetNumber *unused;

			unused = (OffsetNumber *) ((char *) xldata + SizeOfHashVacuumOnePage);

			PageIndexMultiDelete(page, unused, xldata->ntuples);
		}

		/*
		 * Mark the page as not containing any LP_DEAD items. See comments in
		 * _hash_vacuum_one_page() for details.
		 */
		pageopaque = (HashPageOpaque) PageGetSpecialPointer(page);
		pageopaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;

		PageSetLSN(page, lsn);
		MarkBufferDirty(buffer);
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);

	if (XLogReadBufferForRedo(record, 1, &metabuf) == BLK_NEEDS_REDO)
	{
		Page		metapage;
		HashMetaPage metap;

		metapage = BufferGetPage(metabuf);
		metap = HashPageGetMeta(metapage);

		metap->hashm_ntuples -= xldata->ntuples;

		PageSetLSN(metapage, lsn);
		MarkBufferDirty(metabuf);
	}
	if (BufferIsValid(metabuf))
		UnlockReleaseBuffer(metabuf);
}

void
hash_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_HASH_INIT_META_PAGE:
			hash_xlog_init_meta_page(record);
			break;
		case XLOG_HASH_INIT_BITMAP_PAGE:
			hash_xlog_init_bitmap_page(record);
			break;
		case XLOG_HASH_INSERT:
			hash_xlog_insert(record);
			break;
		case XLOG_HASH_ADD_OVFL_PAGE:
			hash_xlog_add_ovfl_page(record);
			break;
		case XLOG_HASH_SPLIT_ALLOCATE_PAGE:
			hash_xlog_split_allocate_page(record);
			break;
		case XLOG_HASH_SPLIT_PAGE:
			hash_xlog_split_page(record);
			break;
		case XLOG_HASH_SPLIT_COMPLETE:
			hash_xlog_split_complete(record);
			break;
		case XLOG_HASH_MOVE_PAGE_CONTENTS:
			hash_xlog_move_page_contents(record);
			break;
		case XLOG_HASH_SQUEEZE_PAGE:
			hash_xlog_squeeze_page(record);
			break;
		case XLOG_HASH_DELETE:
			hash_xlog_delete(record);
			break;
		case XLOG_HASH_SPLIT_CLEANUP:
			hash_xlog_split_cleanup(record);
			break;
		case XLOG_HASH_UPDATE_META_PAGE:
			hash_xlog_update_meta_page(record);
			break;
		case XLOG_HASH_VACUUM_ONE_PAGE:
			hash_xlog_vacuum_one_page(record);
			break;
		default:
			elog(PANIC, "hash_redo: unknown op code %u", info);
	}
}

/*
 * Mask a hash page before performing consistency checks on it.
 */
void
hash_mask(char *pagedata, BlockNumber blkno)
{
	Page		page = (Page) pagedata;
	HashPageOpaque opaque;
	int			pagetype;

	mask_page_lsn_and_checksum(page);

	mask_page_hint_bits(page);
	mask_unused_space(page);

	opaque = (HashPageOpaque) PageGetSpecialPointer(page);

	pagetype = opaque->hasho_flag & LH_PAGE_TYPE;
	if (pagetype == LH_UNUSED_PAGE)
	{
		/*
		 * Mask everything on a UNUSED page.
		 */
		mask_page_content(page);
	}
	else if (pagetype == LH_BUCKET_PAGE ||
			 pagetype == LH_OVERFLOW_PAGE)
	{
		/*
		 * In hash bucket and overflow pages, it is possible to modify the
		 * LP_FLAGS without emitting any WAL record. Hence, mask the line
		 * pointer flags. See hashgettuple(), _hash_kill_items() for details.
		 */
		mask_lp_flags(page);
	}

	/*
	 * It is possible that the hint bit LH_PAGE_HAS_DEAD_TUPLES may remain
	 * unlogged. So, mask it. See _hash_kill_items() for details.
	 */
	opaque->hasho_flag &= ~LH_PAGE_HAS_DEAD_TUPLES;
}
