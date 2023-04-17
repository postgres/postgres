/*-------------------------------------------------------------------------
 *
 * xloginsert.c
 *		Functions for constructing WAL records
 *
 * Constructing a WAL record begins with a call to XLogBeginInsert,
 * followed by a number of XLogRegister* calls. The registered data is
 * collected in private working memory, and finally assembled into a chain
 * of XLogRecData structs by a call to XLogRecordAssemble(). See
 * access/transam/README for details.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xloginsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "replication/origin.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"
#include "pg_trace.h"

/* Buffer size required to store a compressed version of backup block image */
#define PGLZ_MAX_BLCKSZ PGLZ_MAX_OUTPUT(BLCKSZ)

/*
 * For each block reference registered with XLogRegisterBuffer, we fill in
 * a registered_buffer struct.
 */
typedef struct
{
	bool		in_use;			/* is this slot in use? */
	uint8		flags;			/* REGBUF_* flags */
	RelFileNode rnode;			/* identifies the relation and block */
	ForkNumber	forkno;
	BlockNumber block;
	Page		page;			/* page content */
	uint32		rdata_len;		/* total length of data in rdata chain */
	XLogRecData *rdata_head;	/* head of the chain of data registered with
								 * this block */
	XLogRecData *rdata_tail;	/* last entry in the chain, or &rdata_head if
								 * empty */

	XLogRecData bkp_rdatas[2];	/* temporary rdatas used to hold references to
								 * backup block data in XLogRecordAssemble() */

	/* buffer to store a compressed version of backup block image */
	char		compressed_page[PGLZ_MAX_BLCKSZ];
} registered_buffer;

static registered_buffer *registered_buffers;
static int	max_registered_buffers; /* allocated size */
static int	max_registered_block_id = 0;	/* highest block_id + 1 currently
											 * registered */

/*
 * A chain of XLogRecDatas to hold the "main data" of a WAL record, registered
 * with XLogRegisterData(...).
 */
static XLogRecData *mainrdata_head;
static XLogRecData *mainrdata_last = (XLogRecData *) &mainrdata_head;
static uint32 mainrdata_len;	/* total # of bytes in chain */

/* flags for the in-progress insertion */
static uint8 curinsert_flags = 0;

/*
 * These are used to hold the record header while constructing a record.
 * 'hdr_scratch' is not a plain variable, but is palloc'd at initialization,
 * because we want it to be MAXALIGNed and padding bytes zeroed.
 *
 * For simplicity, it's allocated large enough to hold the headers for any
 * WAL record.
 */
static XLogRecData hdr_rdt;
static char *hdr_scratch = NULL;

#define SizeOfXlogOrigin	(sizeof(RepOriginId) + sizeof(char))

#define HEADER_SCRATCH_SIZE \
	(SizeOfXLogRecord + \
	 MaxSizeOfXLogRecordBlockHeader * (XLR_MAX_BLOCK_ID + 1) + \
	 SizeOfXLogRecordDataHeaderLong + SizeOfXlogOrigin)

/*
 * An array of XLogRecData structs, to hold registered data.
 */
static XLogRecData *rdatas;
static int	num_rdatas;			/* entries currently used */
static int	max_rdatas;			/* allocated size */

static bool begininsert_called = false;

/* Memory context to hold the registered buffer and data references. */
static MemoryContext xloginsert_cxt;

static XLogRecData *XLogRecordAssemble(RmgrId rmid, uint8 info,
									   XLogRecPtr RedoRecPtr, bool doPageWrites,
									   XLogRecPtr *fpw_lsn);
static bool XLogCompressBackupBlock(char *page, uint16 hole_offset,
									uint16 hole_length, char *dest, uint16 *dlen);

/*
 * Begin constructing a WAL record. This must be called before the
 * XLogRegister* functions and XLogInsert().
 */
void
XLogBeginInsert(void)
{
	Assert(max_registered_block_id == 0);
	Assert(mainrdata_last == (XLogRecData *) &mainrdata_head);
	Assert(mainrdata_len == 0);

	/* cross-check on whether we should be here or not */
	if (!XLogInsertAllowed())
		elog(ERROR, "cannot make new WAL entries during recovery");

	if (begininsert_called)
		elog(ERROR, "XLogBeginInsert was already called");

	begininsert_called = true;
}

/*
 * Ensure that there are enough buffer and data slots in the working area,
 * for subsequent XLogRegisterBuffer, XLogRegisterData and XLogRegisterBufData
 * calls.
 *
 * There is always space for a small number of buffers and data chunks, enough
 * for most record types. This function is for the exceptional cases that need
 * more.
 */
void
XLogEnsureRecordSpace(int max_block_id, int ndatas)
{
	int			nbuffers;

	/*
	 * This must be called before entering a critical section, because
	 * allocating memory inside a critical section can fail. repalloc() will
	 * check the same, but better to check it here too so that we fail
	 * consistently even if the arrays happen to be large enough already.
	 */
	Assert(CritSectionCount == 0);

	/* the minimum values can't be decreased */
	if (max_block_id < XLR_NORMAL_MAX_BLOCK_ID)
		max_block_id = XLR_NORMAL_MAX_BLOCK_ID;
	if (ndatas < XLR_NORMAL_RDATAS)
		ndatas = XLR_NORMAL_RDATAS;

	if (max_block_id > XLR_MAX_BLOCK_ID)
		elog(ERROR, "maximum number of WAL record block references exceeded");
	nbuffers = max_block_id + 1;

	if (nbuffers > max_registered_buffers)
	{
		registered_buffers = (registered_buffer *)
			repalloc(registered_buffers, sizeof(registered_buffer) * nbuffers);

		/*
		 * At least the padding bytes in the structs must be zeroed, because
		 * they are included in WAL data, but initialize it all for tidiness.
		 */
		MemSet(&registered_buffers[max_registered_buffers], 0,
			   (nbuffers - max_registered_buffers) * sizeof(registered_buffer));
		max_registered_buffers = nbuffers;
	}

	if (ndatas > max_rdatas)
	{
		rdatas = (XLogRecData *) repalloc(rdatas, sizeof(XLogRecData) * ndatas);
		max_rdatas = ndatas;
	}
}

/*
 * Reset WAL record construction buffers.
 */
void
XLogResetInsertion(void)
{
	int			i;

	for (i = 0; i < max_registered_block_id; i++)
		registered_buffers[i].in_use = false;

	num_rdatas = 0;
	max_registered_block_id = 0;
	mainrdata_len = 0;
	mainrdata_last = (XLogRecData *) &mainrdata_head;
	curinsert_flags = 0;
	begininsert_called = false;
}

/*
 * Register a reference to a buffer with the WAL record being constructed.
 * This must be called for every page that the WAL-logged operation modifies.
 */
void
XLogRegisterBuffer(uint8 block_id, Buffer buffer, uint8 flags)
{
	registered_buffer *regbuf;

	/* NO_IMAGE doesn't make sense with FORCE_IMAGE */
	Assert(!((flags & REGBUF_FORCE_IMAGE) && (flags & (REGBUF_NO_IMAGE))));
	Assert(begininsert_called);

	if (block_id >= max_registered_block_id)
	{
		if (block_id >= max_registered_buffers)
			elog(ERROR, "too many registered buffers");
		max_registered_block_id = block_id + 1;
	}

	regbuf = &registered_buffers[block_id];

	BufferGetTag(buffer, &regbuf->rnode, &regbuf->forkno, &regbuf->block);
	regbuf->page = BufferGetPage(buffer);
	regbuf->flags = flags;
	regbuf->rdata_tail = (XLogRecData *) &regbuf->rdata_head;
	regbuf->rdata_len = 0;

	/*
	 * Check that this page hasn't already been registered with some other
	 * block_id.
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < max_registered_block_id; i++)
		{
			registered_buffer *regbuf_old = &registered_buffers[i];

			if (i == block_id || !regbuf_old->in_use)
				continue;

			Assert(!RelFileNodeEquals(regbuf_old->rnode, regbuf->rnode) ||
				   regbuf_old->forkno != regbuf->forkno ||
				   regbuf_old->block != regbuf->block);
		}
	}
#endif

	regbuf->in_use = true;
}

/*
 * Like XLogRegisterBuffer, but for registering a block that's not in the
 * shared buffer pool (i.e. when you don't have a Buffer for it).
 */
void
XLogRegisterBlock(uint8 block_id, RelFileNode *rnode, ForkNumber forknum,
				  BlockNumber blknum, Page page, uint8 flags)
{
	registered_buffer *regbuf;

	/* This is currently only used to WAL-log a full-page image of a page */
	Assert(flags & REGBUF_FORCE_IMAGE);
	Assert(begininsert_called);

	if (block_id >= max_registered_block_id)
		max_registered_block_id = block_id + 1;

	if (block_id >= max_registered_buffers)
		elog(ERROR, "too many registered buffers");

	regbuf = &registered_buffers[block_id];

	regbuf->rnode = *rnode;
	regbuf->forkno = forknum;
	regbuf->block = blknum;
	regbuf->page = page;
	regbuf->flags = flags;
	regbuf->rdata_tail = (XLogRecData *) &regbuf->rdata_head;
	regbuf->rdata_len = 0;

	/*
	 * Check that this page hasn't already been registered with some other
	 * block_id.
	 */
#ifdef USE_ASSERT_CHECKING
	{
		int			i;

		for (i = 0; i < max_registered_block_id; i++)
		{
			registered_buffer *regbuf_old = &registered_buffers[i];

			if (i == block_id || !regbuf_old->in_use)
				continue;

			Assert(!RelFileNodeEquals(regbuf_old->rnode, regbuf->rnode) ||
				   regbuf_old->forkno != regbuf->forkno ||
				   regbuf_old->block != regbuf->block);
		}
	}
#endif

	regbuf->in_use = true;
}

/*
 * Add data to the WAL record that's being constructed.
 *
 * The data is appended to the "main chunk", available at replay with
 * XLogRecGetData().
 */
void
XLogRegisterData(char *data, int len)
{
	XLogRecData *rdata;

	Assert(begininsert_called);

	if (num_rdatas >= max_rdatas)
		elog(ERROR, "too much WAL data");
	rdata = &rdatas[num_rdatas++];

	rdata->data = data;
	rdata->len = len;

	/*
	 * we use the mainrdata_last pointer to track the end of the chain, so no
	 * need to clear 'next' here.
	 */

	mainrdata_last->next = rdata;
	mainrdata_last = rdata;

	mainrdata_len += len;
}

/*
 * Add buffer-specific data to the WAL record that's being constructed.
 *
 * Block_id must reference a block previously registered with
 * XLogRegisterBuffer(). If this is called more than once for the same
 * block_id, the data is appended.
 *
 * The maximum amount of data that can be registered per block is 65535
 * bytes. That should be plenty; if you need more than BLCKSZ bytes to
 * reconstruct the changes to the page, you might as well just log a full
 * copy of it. (the "main data" that's not associated with a block is not
 * limited)
 */
void
XLogRegisterBufData(uint8 block_id, char *data, int len)
{
	registered_buffer *regbuf;
	XLogRecData *rdata;

	Assert(begininsert_called);

	/* find the registered buffer struct */
	regbuf = &registered_buffers[block_id];
	if (!regbuf->in_use)
		elog(ERROR, "no block with id %d registered with WAL insertion",
			 block_id);

	if (num_rdatas >= max_rdatas)
		elog(ERROR, "too much WAL data");
	rdata = &rdatas[num_rdatas++];

	rdata->data = data;
	rdata->len = len;

	regbuf->rdata_tail->next = rdata;
	regbuf->rdata_tail = rdata;
	regbuf->rdata_len += len;
}

/*
 * Set insert status flags for the upcoming WAL record.
 *
 * The flags that can be used here are:
 * - XLOG_INCLUDE_ORIGIN, to determine if the replication origin should be
 *	 included in the record.
 * - XLOG_MARK_UNIMPORTANT, to signal that the record is not important for
 *	 durability, which allows to avoid triggering WAL archiving and other
 *	 background activity.
 */
void
XLogSetRecordFlags(uint8 flags)
{
	Assert(begininsert_called);
	curinsert_flags = flags;
}

/*
 * Insert an XLOG record having the specified RMID and info bytes, with the
 * body of the record being the data and buffer references registered earlier
 * with XLogRegister* calls.
 *
 * Returns XLOG pointer to end of record (beginning of next record).
 * This can be used as LSN for data pages affected by the logged action.
 * (LSN is the XLOG point up to which the XLOG must be flushed to disk
 * before the data page can be written out.  This implements the basic
 * WAL rule "write the log before the data".)
 */
XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info)
{
	XLogRecPtr	EndPos;

	/* XLogBeginInsert() must have been called. */
	if (!begininsert_called)
		elog(ERROR, "XLogBeginInsert was not called");

	/*
	 * The caller can set rmgr bits, XLR_SPECIAL_REL_UPDATE and
	 * XLR_CHECK_CONSISTENCY; the rest are reserved for use by me.
	 */
	if ((info & ~(XLR_RMGR_INFO_MASK |
				  XLR_SPECIAL_REL_UPDATE |
				  XLR_CHECK_CONSISTENCY)) != 0)
		elog(PANIC, "invalid xlog info mask %02X", info);

	TRACE_POSTGRESQL_WAL_INSERT(rmid, info);

	/*
	 * In bootstrap mode, we don't actually log anything but XLOG resources;
	 * return a phony record pointer.
	 */
	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		XLogResetInsertion();
		EndPos = SizeOfXLogLongPHD; /* start of 1st chkpt record */
		return EndPos;
	}

	do
	{
		XLogRecPtr	RedoRecPtr;
		bool		doPageWrites;
		XLogRecPtr	fpw_lsn;
		XLogRecData *rdt;

		/*
		 * Get values needed to decide whether to do full-page writes. Since
		 * we don't yet have an insertion lock, these could change under us,
		 * but XLogInsertRecord will recheck them once it has a lock.
		 */
		GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

		rdt = XLogRecordAssemble(rmid, info, RedoRecPtr, doPageWrites,
								 &fpw_lsn);

		EndPos = XLogInsertRecord(rdt, fpw_lsn, curinsert_flags);
	} while (EndPos == InvalidXLogRecPtr);

	XLogResetInsertion();

	return EndPos;
}

/*
 * Assemble a WAL record from the registered data and buffers into an
 * XLogRecData chain, ready for insertion with XLogInsertRecord().
 *
 * The record header fields are filled in, except for the xl_prev field. The
 * calculated CRC does not include the record header yet.
 *
 * If there are any registered buffers, and a full-page image was not taken
 * of all of them, *fpw_lsn is set to the lowest LSN among such pages. This
 * signals that the assembled record is only good for insertion on the
 * assumption that the RedoRecPtr and doPageWrites values were up-to-date.
 */
static XLogRecData *
XLogRecordAssemble(RmgrId rmid, uint8 info,
				   XLogRecPtr RedoRecPtr, bool doPageWrites,
				   XLogRecPtr *fpw_lsn)
{
	XLogRecData *rdt;
	uint32		total_len = 0;
	int			block_id;
	pg_crc32c	rdata_crc;
	registered_buffer *prev_regbuf = NULL;
	XLogRecData *rdt_datas_last;
	XLogRecord *rechdr;
	char	   *scratch = hdr_scratch;

	/*
	 * Note: this function can be called multiple times for the same record.
	 * All the modifications we do to the rdata chains below must handle that.
	 */

	/* The record begins with the fixed-size header */
	rechdr = (XLogRecord *) scratch;
	scratch += SizeOfXLogRecord;

	hdr_rdt.next = NULL;
	rdt_datas_last = &hdr_rdt;
	hdr_rdt.data = hdr_scratch;

	/*
	 * Enforce consistency checks for this record if user is looking for it.
	 * Do this before at the beginning of this routine to give the possibility
	 * for callers of XLogInsert() to pass XLR_CHECK_CONSISTENCY directly for
	 * a record.
	 */
	if (wal_consistency_checking[rmid])
		info |= XLR_CHECK_CONSISTENCY;

	/*
	 * Make an rdata chain containing all the data portions of all block
	 * references. This includes the data for full-page images. Also append
	 * the headers for the block references in the scratch buffer.
	 */
	*fpw_lsn = InvalidXLogRecPtr;
	for (block_id = 0; block_id < max_registered_block_id; block_id++)
	{
		registered_buffer *regbuf = &registered_buffers[block_id];
		bool		needs_backup;
		bool		needs_data;
		XLogRecordBlockHeader bkpb;
		XLogRecordBlockImageHeader bimg;
		XLogRecordBlockCompressHeader cbimg = {0};
		bool		samerel;
		bool		is_compressed = false;
		bool		include_image;

		if (!regbuf->in_use)
			continue;

		/* Determine if this block needs to be backed up */
		if (regbuf->flags & REGBUF_FORCE_IMAGE)
			needs_backup = true;
		else if (regbuf->flags & REGBUF_NO_IMAGE)
			needs_backup = false;
		else if (!doPageWrites)
			needs_backup = false;
		else
		{
			/*
			 * We assume page LSN is first data on *every* page that can be
			 * passed to XLogInsert, whether it has the standard page layout
			 * or not.
			 */
			XLogRecPtr	page_lsn = PageGetLSN(regbuf->page);

			needs_backup = (page_lsn <= RedoRecPtr);
			if (!needs_backup)
			{
				if (*fpw_lsn == InvalidXLogRecPtr || page_lsn < *fpw_lsn)
					*fpw_lsn = page_lsn;
			}
		}

		/* Determine if the buffer data needs to included */
		if (regbuf->rdata_len == 0)
			needs_data = false;
		else if ((regbuf->flags & REGBUF_KEEP_DATA) != 0)
			needs_data = true;
		else
			needs_data = !needs_backup;

		bkpb.id = block_id;
		bkpb.fork_flags = regbuf->forkno;
		bkpb.data_length = 0;

		if ((regbuf->flags & REGBUF_WILL_INIT) == REGBUF_WILL_INIT)
			bkpb.fork_flags |= BKPBLOCK_WILL_INIT;

		/*
		 * If needs_backup is true or WAL checking is enabled for current
		 * resource manager, log a full-page write for the current block.
		 */
		include_image = needs_backup || (info & XLR_CHECK_CONSISTENCY) != 0;

		if (include_image)
		{
			Page		page = regbuf->page;
			uint16		compressed_len = 0;

			/*
			 * The page needs to be backed up, so calculate its hole length
			 * and offset.
			 */
			if (regbuf->flags & REGBUF_STANDARD)
			{
				/* Assume we can omit data between pd_lower and pd_upper */
				uint16		lower = ((PageHeader) page)->pd_lower;
				uint16		upper = ((PageHeader) page)->pd_upper;

				if (lower >= SizeOfPageHeaderData &&
					upper > lower &&
					upper <= BLCKSZ)
				{
					bimg.hole_offset = lower;
					cbimg.hole_length = upper - lower;
				}
				else
				{
					/* No "hole" to remove */
					bimg.hole_offset = 0;
					cbimg.hole_length = 0;
				}
			}
			else
			{
				/* Not a standard page header, don't try to eliminate "hole" */
				bimg.hole_offset = 0;
				cbimg.hole_length = 0;
			}

			/*
			 * Try to compress a block image if wal_compression is enabled
			 */
			if (wal_compression)
			{
				is_compressed =
					XLogCompressBackupBlock(page, bimg.hole_offset,
											cbimg.hole_length,
											regbuf->compressed_page,
											&compressed_len);
			}

			/*
			 * Fill in the remaining fields in the XLogRecordBlockHeader
			 * struct
			 */
			bkpb.fork_flags |= BKPBLOCK_HAS_IMAGE;

			/*
			 * Construct XLogRecData entries for the page content.
			 */
			rdt_datas_last->next = &regbuf->bkp_rdatas[0];
			rdt_datas_last = rdt_datas_last->next;

			bimg.bimg_info = (cbimg.hole_length == 0) ? 0 : BKPIMAGE_HAS_HOLE;

			/*
			 * If WAL consistency checking is enabled for the resource manager
			 * of this WAL record, a full-page image is included in the record
			 * for the block modified. During redo, the full-page is replayed
			 * only if BKPIMAGE_APPLY is set.
			 */
			if (needs_backup)
				bimg.bimg_info |= BKPIMAGE_APPLY;

			if (is_compressed)
			{
				bimg.length = compressed_len;
				bimg.bimg_info |= BKPIMAGE_IS_COMPRESSED;

				rdt_datas_last->data = regbuf->compressed_page;
				rdt_datas_last->len = compressed_len;
			}
			else
			{
				bimg.length = BLCKSZ - cbimg.hole_length;

				if (cbimg.hole_length == 0)
				{
					rdt_datas_last->data = page;
					rdt_datas_last->len = BLCKSZ;
				}
				else
				{
					/* must skip the hole */
					rdt_datas_last->data = page;
					rdt_datas_last->len = bimg.hole_offset;

					rdt_datas_last->next = &regbuf->bkp_rdatas[1];
					rdt_datas_last = rdt_datas_last->next;

					rdt_datas_last->data =
						page + (bimg.hole_offset + cbimg.hole_length);
					rdt_datas_last->len =
						BLCKSZ - (bimg.hole_offset + cbimg.hole_length);
				}
			}

			total_len += bimg.length;
		}

		if (needs_data)
		{
			/*
			 * Link the caller-supplied rdata chain for this buffer to the
			 * overall list.
			 */
			bkpb.fork_flags |= BKPBLOCK_HAS_DATA;
			bkpb.data_length = regbuf->rdata_len;
			total_len += regbuf->rdata_len;

			rdt_datas_last->next = regbuf->rdata_head;
			rdt_datas_last = regbuf->rdata_tail;
		}

		if (prev_regbuf && RelFileNodeEquals(regbuf->rnode, prev_regbuf->rnode))
		{
			samerel = true;
			bkpb.fork_flags |= BKPBLOCK_SAME_REL;
		}
		else
			samerel = false;
		prev_regbuf = regbuf;

		/* Ok, copy the header to the scratch buffer */
		memcpy(scratch, &bkpb, SizeOfXLogRecordBlockHeader);
		scratch += SizeOfXLogRecordBlockHeader;
		if (include_image)
		{
			memcpy(scratch, &bimg, SizeOfXLogRecordBlockImageHeader);
			scratch += SizeOfXLogRecordBlockImageHeader;
			if (cbimg.hole_length != 0 && is_compressed)
			{
				memcpy(scratch, &cbimg,
					   SizeOfXLogRecordBlockCompressHeader);
				scratch += SizeOfXLogRecordBlockCompressHeader;
			}
		}
		if (!samerel)
		{
			memcpy(scratch, &regbuf->rnode, sizeof(RelFileNode));
			scratch += sizeof(RelFileNode);
		}
		memcpy(scratch, &regbuf->block, sizeof(BlockNumber));
		scratch += sizeof(BlockNumber);
	}

	/* followed by the record's origin, if any */
	if ((curinsert_flags & XLOG_INCLUDE_ORIGIN) &&
		replorigin_session_origin != InvalidRepOriginId)
	{
		*(scratch++) = (char) XLR_BLOCK_ID_ORIGIN;
		memcpy(scratch, &replorigin_session_origin, sizeof(replorigin_session_origin));
		scratch += sizeof(replorigin_session_origin);
	}

	/* followed by main data, if any */
	if (mainrdata_len > 0)
	{
		if (mainrdata_len > 255)
		{
			*(scratch++) = (char) XLR_BLOCK_ID_DATA_LONG;
			memcpy(scratch, &mainrdata_len, sizeof(uint32));
			scratch += sizeof(uint32);
		}
		else
		{
			*(scratch++) = (char) XLR_BLOCK_ID_DATA_SHORT;
			*(scratch++) = (uint8) mainrdata_len;
		}
		rdt_datas_last->next = mainrdata_head;
		rdt_datas_last = mainrdata_last;
		total_len += mainrdata_len;
	}
	rdt_datas_last->next = NULL;

	hdr_rdt.len = (scratch - hdr_scratch);
	total_len += hdr_rdt.len;

	/*
	 * Calculate CRC of the data
	 *
	 * Note that the record header isn't added into the CRC initially since we
	 * don't know the prev-link yet.  Thus, the CRC will represent the CRC of
	 * the whole record in the order: rdata, then backup blocks, then record
	 * header.
	 */
	INIT_CRC32C(rdata_crc);
	COMP_CRC32C(rdata_crc, hdr_scratch + SizeOfXLogRecord, hdr_rdt.len - SizeOfXLogRecord);
	for (rdt = hdr_rdt.next; rdt != NULL; rdt = rdt->next)
		COMP_CRC32C(rdata_crc, rdt->data, rdt->len);

	/*
	 * Fill in the fields in the record header. Prev-link is filled in later,
	 * once we know where in the WAL the record will be inserted. The CRC does
	 * not include the record header yet.
	 */
	rechdr->xl_xid = GetCurrentTransactionIdIfAny();
	rechdr->xl_tot_len = total_len;
	rechdr->xl_info = info;
	rechdr->xl_rmid = rmid;
	rechdr->xl_prev = InvalidXLogRecPtr;
	rechdr->xl_crc = rdata_crc;

	return &hdr_rdt;
}

/*
 * Create a compressed version of a backup block image.
 *
 * Returns false if compression fails (i.e., compressed result is actually
 * bigger than original). Otherwise, returns true and sets 'dlen' to
 * the length of compressed block image.
 */
static bool
XLogCompressBackupBlock(char *page, uint16 hole_offset, uint16 hole_length,
						char *dest, uint16 *dlen)
{
	int32		orig_len = BLCKSZ - hole_length;
	int32		len;
	int32		extra_bytes = 0;
	char	   *source;
	PGAlignedBlock tmp;

	if (hole_length != 0)
	{
		/* must skip the hole */
		source = tmp.data;
		memcpy(source, page, hole_offset);
		memcpy(source + hole_offset,
			   page + (hole_offset + hole_length),
			   BLCKSZ - (hole_length + hole_offset));

		/*
		 * Extra data needs to be stored in WAL record for the compressed
		 * version of block image if the hole exists.
		 */
		extra_bytes = SizeOfXLogRecordBlockCompressHeader;
	}
	else
		source = page;

	/*
	 * We recheck the actual size even if pglz_compress() reports success and
	 * see if the number of bytes saved by compression is larger than the
	 * length of extra data needed for the compressed version of block image.
	 */
	len = pglz_compress(source, orig_len, dest, PGLZ_strategy_default);
	if (len >= 0 &&
		len + extra_bytes < orig_len)
	{
		*dlen = (uint16) len;	/* successful compression */
		return true;
	}
	return false;
}

/*
 * Determine whether the buffer referenced has to be backed up.
 *
 * Since we don't yet have the insert lock, fullPageWrites and forcePageWrites
 * could change later, so the result should be used for optimization purposes
 * only.
 */
bool
XLogCheckBufferNeedsBackup(Buffer buffer)
{
	XLogRecPtr	RedoRecPtr;
	bool		doPageWrites;
	Page		page;

	GetFullPageWriteInfo(&RedoRecPtr, &doPageWrites);

	page = BufferGetPage(buffer);

	if (doPageWrites && PageGetLSN(page) <= RedoRecPtr)
		return true;			/* buffer requires backup */

	return false;				/* buffer does not need to be backed up */
}

/*
 * Write a backup block if needed when we are setting a hint. Note that
 * this may be called for a variety of page types, not just heaps.
 *
 * Callable while holding just share lock on the buffer content.
 *
 * We can't use the plain backup block mechanism since that relies on the
 * Buffer being exclusively locked. Since some modifications (setting LSN, hint
 * bits) are allowed in a sharelocked buffer that can lead to wal checksum
 * failures. So instead we copy the page and insert the copied data as normal
 * record data.
 *
 * We only need to do something if page has not yet been full page written in
 * this checkpoint round. The LSN of the inserted wal record is returned if we
 * had to write, InvalidXLogRecPtr otherwise.
 *
 * It is possible that multiple concurrent backends could attempt to write WAL
 * records. In that case, multiple copies of the same block would be recorded
 * in separate WAL records by different backends, though that is still OK from
 * a correctness perspective.
 */
XLogRecPtr
XLogSaveBufferForHint(Buffer buffer, bool buffer_std)
{
	XLogRecPtr	recptr = InvalidXLogRecPtr;
	XLogRecPtr	lsn;
	XLogRecPtr	RedoRecPtr;

	/*
	 * Ensure no checkpoint can change our view of RedoRecPtr.
	 */
	Assert(MyPgXact->delayChkpt);

	/*
	 * Update RedoRecPtr so that we can make the right decision
	 */
	RedoRecPtr = GetRedoRecPtr();

	/*
	 * We assume page LSN is first data on *every* page that can be passed to
	 * XLogInsert, whether it has the standard page layout or not. Since we're
	 * only holding a share-lock on the page, we must take the buffer header
	 * lock when we look at the LSN.
	 */
	lsn = BufferGetLSNAtomic(buffer);

	if (lsn <= RedoRecPtr)
	{
		int			flags;
		PGAlignedBlock copied_buffer;
		char	   *origdata = (char *) BufferGetBlock(buffer);
		RelFileNode rnode;
		ForkNumber	forkno;
		BlockNumber blkno;

		/*
		 * Copy buffer so we don't have to worry about concurrent hint bit or
		 * lsn updates. We assume pd_lower/upper cannot be changed without an
		 * exclusive lock, so the contents bkp are not racy.
		 */
		if (buffer_std)
		{
			/* Assume we can omit data between pd_lower and pd_upper */
			Page		page = BufferGetPage(buffer);
			uint16		lower = ((PageHeader) page)->pd_lower;
			uint16		upper = ((PageHeader) page)->pd_upper;

			memcpy(copied_buffer.data, origdata, lower);
			memcpy(copied_buffer.data + upper, origdata + upper, BLCKSZ - upper);
		}
		else
			memcpy(copied_buffer.data, origdata, BLCKSZ);

		XLogBeginInsert();

		flags = REGBUF_FORCE_IMAGE;
		if (buffer_std)
			flags |= REGBUF_STANDARD;

		BufferGetTag(buffer, &rnode, &forkno, &blkno);
		XLogRegisterBlock(0, &rnode, forkno, blkno, copied_buffer.data, flags);

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI_FOR_HINT);
	}

	return recptr;
}

/*
 * Write a WAL record containing a full image of a page. Caller is responsible
 * for writing the page to disk after calling this routine.
 *
 * Note: If you're using this function, you should be building pages in private
 * memory and writing them directly to smgr.  If you're using buffers, call
 * log_newpage_buffer instead.
 *
 * If the page follows the standard page layout, with a PageHeader and unused
 * space between pd_lower and pd_upper, set 'page_std' to true. That allows
 * the unused space to be left out from the WAL record, making it smaller.
 */
XLogRecPtr
log_newpage(RelFileNode *rnode, ForkNumber forkNum, BlockNumber blkno,
			Page page, bool page_std)
{
	int			flags;
	XLogRecPtr	recptr;

	flags = REGBUF_FORCE_IMAGE;
	if (page_std)
		flags |= REGBUF_STANDARD;

	XLogBeginInsert();
	XLogRegisterBlock(0, rnode, forkNum, blkno, page, flags);
	recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);

	/*
	 * The page may be uninitialized. If so, we can't set the LSN because that
	 * would corrupt the page.
	 */
	if (!PageIsNew(page))
	{
		PageSetLSN(page, recptr);
	}

	return recptr;
}

/*
 * Write a WAL record containing a full image of a page.
 *
 * Caller should initialize the buffer and mark it dirty before calling this
 * function.  This function will set the page LSN.
 *
 * If the page follows the standard page layout, with a PageHeader and unused
 * space between pd_lower and pd_upper, set 'page_std' to true. That allows
 * the unused space to be left out from the WAL record, making it smaller.
 */
XLogRecPtr
log_newpage_buffer(Buffer buffer, bool page_std)
{
	Page		page = BufferGetPage(buffer);
	RelFileNode rnode;
	ForkNumber	forkNum;
	BlockNumber blkno;

	/* Shared buffers should be modified in a critical section. */
	Assert(CritSectionCount > 0);

	BufferGetTag(buffer, &rnode, &forkNum, &blkno);

	return log_newpage(&rnode, forkNum, blkno, page, page_std);
}

/*
 * WAL-log a range of blocks in a relation.
 *
 * An image of all pages with block numbers 'startblk' <= X < 'endblk' is
 * written to the WAL. If the range is large, this is done in multiple WAL
 * records.
 *
 * If all page follows the standard page layout, with a PageHeader and unused
 * space between pd_lower and pd_upper, set 'page_std' to true. That allows
 * the unused space to be left out from the WAL records, making them smaller.
 *
 * NOTE: This function acquires exclusive-locks on the pages. Typically, this
 * is used on a newly-built relation, and the caller is holding a
 * AccessExclusiveLock on it, so no other backend can be accessing it at the
 * same time. If that's not the case, you must ensure that this does not
 * cause a deadlock through some other means.
 */
void
log_newpage_range(Relation rel, ForkNumber forkNum,
				  BlockNumber startblk, BlockNumber endblk,
				  bool page_std)
{
	int			flags;
	BlockNumber blkno;

	flags = REGBUF_FORCE_IMAGE;
	if (page_std)
		flags |= REGBUF_STANDARD;

	/*
	 * Iterate over all the pages in the range. They are collected into
	 * batches of XLR_MAX_BLOCK_ID pages, and a single WAL-record is written
	 * for each batch.
	 */
	XLogEnsureRecordSpace(XLR_MAX_BLOCK_ID - 1, 0);

	blkno = startblk;
	while (blkno < endblk)
	{
		Buffer		bufpack[XLR_MAX_BLOCK_ID];
		XLogRecPtr	recptr;
		int			nbufs;
		int			i;

		CHECK_FOR_INTERRUPTS();

		/* Collect a batch of blocks. */
		nbufs = 0;
		while (nbufs < XLR_MAX_BLOCK_ID && blkno < endblk)
		{
			Buffer		buf = ReadBufferExtended(rel, forkNum, blkno,
												 RBM_NORMAL, NULL);

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			/*
			 * Completely empty pages are not WAL-logged. Writing a WAL record
			 * would change the LSN, and we don't want that. We want the page
			 * to stay empty.
			 */
			if (!PageIsNew(BufferGetPage(buf)))
				bufpack[nbufs++] = buf;
			else
				UnlockReleaseBuffer(buf);
			blkno++;
		}

		/* Nothing more to do if all remaining blocks were empty. */
		if (nbufs == 0)
			break;

		/* Write WAL record for this batch. */
		XLogBeginInsert();

		START_CRIT_SECTION();
		for (i = 0; i < nbufs; i++)
		{
			XLogRegisterBuffer(i, bufpack[i], flags);
			MarkBufferDirty(bufpack[i]);
		}

		recptr = XLogInsert(RM_XLOG_ID, XLOG_FPI);

		for (i = 0; i < nbufs; i++)
		{
			PageSetLSN(BufferGetPage(bufpack[i]), recptr);
			UnlockReleaseBuffer(bufpack[i]);
		}
		END_CRIT_SECTION();
	}
}

/*
 * Allocate working buffers needed for WAL record construction.
 */
void
InitXLogInsert(void)
{
	/* Initialize the working areas */
	if (xloginsert_cxt == NULL)
	{
		xloginsert_cxt = AllocSetContextCreate(TopMemoryContext,
											   "WAL record construction",
											   ALLOCSET_DEFAULT_SIZES);
	}

	if (registered_buffers == NULL)
	{
		registered_buffers = (registered_buffer *)
			MemoryContextAllocZero(xloginsert_cxt,
								   sizeof(registered_buffer) * (XLR_NORMAL_MAX_BLOCK_ID + 1));
		max_registered_buffers = XLR_NORMAL_MAX_BLOCK_ID + 1;
	}
	if (rdatas == NULL)
	{
		rdatas = MemoryContextAlloc(xloginsert_cxt,
									sizeof(XLogRecData) * XLR_NORMAL_RDATAS);
		max_rdatas = XLR_NORMAL_RDATAS;
	}

	/*
	 * Allocate a buffer to hold the header information for a WAL record.
	 */
	if (hdr_scratch == NULL)
		hdr_scratch = MemoryContextAllocZero(xloginsert_cxt,
											 HEADER_SCRATCH_SIZE);
}
