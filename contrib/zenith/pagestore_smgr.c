/*-------------------------------------------------------------------------
 *
 * pagestore_smgr.c
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  contrib/zenith/pagestore_smgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlog_internal.h"
#include "pagestore_client.h"
#include "storage/relfilenode.h"
#include "storage/smgr.h"
#include "access/xlogdefs.h"
#include "storage/bufmgr.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "replication/walsender.h"
#include "catalog/pg_tablespace_d.h"

/*
 * If DEBUG_COMPARE_LOCAL is defined, we pass through all the SMGR API
 * calls to md.c, and *also* do the calls to the Page Server. On every
 * read, compare the versions we read from local disk and Page Server,
 * and Assert that they are identical.
 */
/* #define DEBUG_COMPARE_LOCAL */

#ifdef DEBUG_COMPARE_LOCAL
#include "access/nbtree.h"
#include "storage/bufpage.h"
#include "storage/md.h"
#include "access/xlog_internal.h"

static char *hexdump_page(char *page);
#endif

const int SmgrTrace = DEBUG5;

bool loaded = false;

page_server_api *page_server;

/* GUCs */
char *page_server_connstring;
char *callmemaybe_connstring;
char *zenith_timeline;
bool wal_redo = false;

char const *const ZenithMessageStr[] =
{
	"ZenithExistsRequest",
	"ZenithNblocksRequest",
	"ZenithReadRequest",
	"ZenithStatusResponse",
	"ZenithReadResponse",
	"ZenithNblocksResponse",
};

StringInfoData
zm_pack(ZenithMessage *msg)
{
	StringInfoData	s;

	initStringInfo(&s);
	pq_sendbyte(&s, msg->tag);

	switch (messageTag(msg))
	{
		/* pagestore_client -> pagestore */
		case T_ZenithExistsRequest:
		case T_ZenithNblocksRequest:
		case T_ZenithReadRequest:
		{
			ZenithRequest *msg_req = (ZenithRequest *) msg;

			pq_sendint32(&s, msg_req->page_key.rnode.spcNode);
			pq_sendint32(&s, msg_req->page_key.rnode.dbNode);
			pq_sendint32(&s, msg_req->page_key.rnode.relNode);
			pq_sendbyte(&s, msg_req->page_key.forknum);
			pq_sendint32(&s, msg_req->page_key.blkno);
			pq_sendint64(&s, msg_req->lsn);

			break;
		}

		/* pagestore -> pagestore_client */
		case T_ZenithStatusResponse:
		case T_ZenithNblocksResponse:
		{
			ZenithResponse *msg_resp = (ZenithResponse *) msg;
			pq_sendbyte(&s, msg_resp->ok);
			pq_sendint32(&s, msg_resp->n_blocks);
			break;
		}
		case T_ZenithReadResponse:
		{
			ZenithResponse *msg_resp = (ZenithResponse *) msg;
			pq_sendbyte(&s, msg_resp->ok);
			pq_sendint32(&s, msg_resp->n_blocks);
			pq_sendbytes(&s, msg_resp->page, BLCKSZ); // XXX: should be varlena
			break;
		}
	}
	return s;
}

ZenithMessage *
zm_unpack(StringInfo s)
{
	ZenithMessageTag tag = pq_getmsgbyte(s);
	ZenithMessage *msg = NULL;

	switch (tag)
	{
		/* pagestore_client -> pagestore */
		case T_ZenithExistsRequest:
		case T_ZenithNblocksRequest:
		case T_ZenithReadRequest:
		{
			ZenithRequest *msg_req = palloc0(sizeof(ZenithRequest));

			msg_req->tag = tag;
			msg_req->system_id = 42;
			msg_req->page_key.rnode.spcNode = pq_getmsgint(s, 4);
			msg_req->page_key.rnode.dbNode = pq_getmsgint(s, 4);
			msg_req->page_key.rnode.relNode = pq_getmsgint(s, 4);
			msg_req->page_key.forknum = pq_getmsgbyte(s);
			msg_req->page_key.blkno = pq_getmsgint(s, 4);
			msg_req->lsn = pq_getmsgint64(s);
			pq_getmsgend(s);

			msg = (ZenithMessage *) msg_req;
			break;
		}

		/* pagestore -> pagestore_client */
		case T_ZenithStatusResponse:
		case T_ZenithNblocksResponse:
		{
			ZenithResponse *msg_resp = palloc0(sizeof(ZenithResponse));

			msg_resp->tag = tag;
			msg_resp->ok = pq_getmsgbyte(s);
			msg_resp->n_blocks = pq_getmsgint(s, 4);
			pq_getmsgend(s);

			msg = (ZenithMessage *) msg_resp;
			break;
		}

		case T_ZenithReadResponse:
		{
			ZenithResponse *msg_resp = palloc0(sizeof(ZenithResponse) + BLCKSZ);

			msg_resp->tag = tag;
			msg_resp->ok = pq_getmsgbyte(s);
			msg_resp->n_blocks = pq_getmsgint(s, 4);
			memcpy(msg_resp->page, pq_getmsgbytes(s, BLCKSZ), BLCKSZ); // XXX: should be varlena
			pq_getmsgend(s);

			msg = (ZenithMessage *) msg_resp;
			break;
		}
	}

	return msg;
}

/* dump to json for debugging / error reporting purposes */
char *
zm_to_string(ZenithMessage *msg)
{
	StringInfoData	s;

	initStringInfo(&s);

	appendStringInfoString(&s, "{");
	appendStringInfo(&s, "\"type\": \"%s\"", ZenithMessageStr[msg->tag]);

	switch (messageTag(msg))
	{
		/* pagestore_client -> pagestore */
		case T_ZenithExistsRequest:
		case T_ZenithNblocksRequest:
		case T_ZenithReadRequest:
		{
			ZenithRequest *msg_req = (ZenithRequest *) msg;

			appendStringInfo(&s, ", \"page_key\": \"%d.%d.%d.%d.%u\", \"lsn\": \"%X/%X\"}",
							 msg_req->page_key.rnode.spcNode,
							 msg_req->page_key.rnode.dbNode,
							 msg_req->page_key.rnode.relNode,
							 msg_req->page_key.forknum,
							 msg_req->page_key.blkno,
							 (uint32) (msg_req->lsn >> 32), (uint32) (msg_req->lsn));

			break;
		}

		/* pagestore -> pagestore_client */
		case T_ZenithStatusResponse:
		case T_ZenithNblocksResponse:
		{
			ZenithResponse *msg_resp = (ZenithResponse *) msg;

			appendStringInfo(&s, ", \"ok\": %d, \"n_blocks\": %u}",
				msg_resp->ok,
				msg_resp->n_blocks
			);

			break;
		}
		case T_ZenithReadResponse:
		{
			ZenithResponse *msg_resp = (ZenithResponse *) msg;

			appendStringInfo(&s, ", \"ok\": %d, \"n_blocks\": %u, \"page\": \"XXX\"}",
				msg_resp->ok,
				msg_resp->n_blocks
			);
			break;
		}
	}
	return s.data;
}


static void
zenith_wallog_page(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, char *buffer)
{
	XLogRecPtr lsn = PageGetLSN(buffer);

	/*
	 * If the page was not WAL-logged before eviction then we can lose its modification.
	 * PD_WAL_LOGGED bit is used to mark pages which are wal-logged.
	 *
	 * See also comments to PD_WAL_LOGGED.
	 *
	 * FIXME: GIN/GiST/SP-GiST index build will scan and WAL-log again the whole index .
	 * That's duplicative with the WAL-logging that we do here.
	 * See log_newpage_range() calls.
	 *
	 * FIXME: Redoing this record will set the LSN on the page. That could
	 * mess up the LSN-NSN interlock in GiST index build.
	 */
	if (forknum == FSM_FORKNUM && !RecoveryInProgress())
	{
		/* FSM is never WAL-logged and we don't care. */
		XLogRecPtr recptr;
		recptr = log_newpage(&reln->smgr_rnode.node, forknum, blocknum, buffer, false);
		XLogFlush(recptr);
		lsn = recptr;
		elog(SmgrTrace, "FSM page %u of relation %u/%u/%u.%u was force logged. Evicted at lsn=%X",
			 blocknum,
			 reln->smgr_rnode.node.spcNode,
			 reln->smgr_rnode.node.dbNode,
			 reln->smgr_rnode.node.relNode,
			 forknum, (uint32)lsn);
	}
	else if (forknum == VISIBILITYMAP_FORKNUM && !RecoveryInProgress())
	{
		/*
		 * Always WAL-log vm.
		 * We should never miss clearing visibility map bits.
		 *
		 * TODO Is it too bad for performance?
		 * Hopefully we do not evict actively used vm too often.
		 */
		XLogRecPtr recptr;
		recptr = log_newpage(&reln->smgr_rnode.node, forknum, blocknum, buffer, false);
		XLogFlush(recptr);
		lsn = recptr;

		elog(SmgrTrace, "Visibilitymap page %u of relation %u/%u/%u.%u was force logged at lsn=%X",
			 blocknum,
			 reln->smgr_rnode.node.spcNode,
			 reln->smgr_rnode.node.dbNode,
			 reln->smgr_rnode.node.relNode,
			 forknum, (uint32)lsn);
	}
	else if (!(((PageHeader)buffer)->pd_flags & PD_WAL_LOGGED)
		&& !RecoveryInProgress())
	{
		XLogRecPtr recptr;
		/*
		 * We assume standard page layout here.
		 *
		 * But at smgr level we don't really know what kind of a page this is.
		 * We have filtered visibility map pages and fsm pages above.
		 * TODO Do we have any special page types?
		 */

		recptr = log_newpage(&reln->smgr_rnode.node, forknum, blocknum, buffer, true);

		/* If we wal-log hint bits, someone could concurrently update page
		 * and reset PD_WAL_LOGGED again, so this assert is not relevant anymore.
		 *
		 * See comment to FlushBuffer().
		 * The caller must hold a pin on the buffer and have share-locked the
		 * buffer contents.  (Note: a share-lock does not prevent updates of
		 * hint bits in the buffer, so the page could change while the write
		 * is in progress, but we assume that that will not invalidate the data
		 * written.)
		 */
		Assert(((PageHeader)buffer)->pd_flags & PD_WAL_LOGGED); /* Should be set by log_newpage */

		/*
		 * Need to flush it too, so that it gets sent to the Page Server before we
		 * might need to read it back. It should get flushed eventually anyway, at
		 * least if there is some other WAL activity, so this isn't strictly
		 * necessary for correctness. But if there is no other WAL activity, the
		 * page read might get stuck waiting for the record to be streamed out
		 * for an indefinite time.
		 *
		 * FIXME: Flushing the WAL is expensive. We should track the last "evicted"
		 * LSN instead, and update it here. Or just kick the bgwriter to do the
		 * flush, there is no need for us to block here waiting for it to finish.
		 */
		XLogFlush(recptr);
		lsn = recptr;
		elog(SmgrTrace, "Force wal logging of page %u of relation %u/%u/%u.%u, lsn=%X",
			 blocknum,
			 reln->smgr_rnode.node.spcNode,
			 reln->smgr_rnode.node.dbNode,
			 reln->smgr_rnode.node.relNode,
			 forknum, (uint32)lsn);
	} else {
		elog(SmgrTrace, "Page %u of relation %u/%u/%u.%u is alread wal logged at lsn=%X",
			 blocknum,
			 reln->smgr_rnode.node.spcNode,
			 reln->smgr_rnode.node.dbNode,
			 reln->smgr_rnode.node.relNode,
			 forknum, (uint32)lsn);
	}
	SetLastWrittenPageLSN(lsn);
}



/*
 *	zenith_init() -- Initialize private state
 */
void
zenith_init(void)
{
	/* noop */
#ifdef DEBUG_COMPARE_LOCAL
	mdinit();
#endif
}

/*
 * GetXLogInsertRecPtr uses XLogBytePosToRecPtr to convert logical insert (reserved) position
 * to physical position in WAL. It always adds SizeOfXLogShortPHD:
 *		seg_offset += fullpages * XLOG_BLCKSZ + bytesleft + SizeOfXLogShortPHD;
 * so even if there are no records on the page, offset will be SizeOfXLogShortPHD.
 * It may cause problems with XLogFlush. So return pointer backward to the origin of the page.
 */
static XLogRecPtr
zm_adjust_lsn(XLogRecPtr lsn)
{
	/* If lsn points to the beging of first record on page or segment,
	 * then "return" it back to the page origin
	 */
	if ((lsn & (XLOG_BLCKSZ-1)) == SizeOfXLogShortPHD)
	{
		lsn -= SizeOfXLogShortPHD;
	}
	else if ((lsn & (wal_segment_size-1)) == SizeOfXLogLongPHD)
	{
		lsn -= SizeOfXLogLongPHD;
	}
	return lsn;
}

/*
 * Return LSN for requesting pages and number of blocks from page server
 */
static XLogRecPtr
zenith_get_request_lsn(bool nonrel)
{
	XLogRecPtr lsn;
	XLogRecPtr flushlsn;

	if (RecoveryInProgress())
	{
		lsn = GetXLogReplayRecPtr(NULL);
		elog(DEBUG1, "zenith_get_request_lsn GetXLogReplayRecPtr %X/%X request lsn 0 ",
			(uint32) ((lsn) >> 32), (uint32) (lsn));

		lsn = InvalidXLogRecPtr;
	}
	else if (am_walsender)
	{
		lsn = InvalidXLogRecPtr;
		elog(DEBUG1, "am walsender zenith_get_request_lsn lsn 0 ");
	}
	else if (nonrel)
	{
		lsn = GetFlushRecPtr();
		elog(DEBUG1, "zenith_get_request_lsn norel GetFlushRecPtr  %X/%X", (uint32) ((lsn) >> 32), (uint32) (lsn));
	}
	else
	{
		flushlsn = GetFlushRecPtr();

		/*
		 * Use the latest LSN that was evicted from the buffer cache. Any
		 * pages modified by later WAL records must still in the buffer cache,
		 * so our request cannot concern those.
		 */
		lsn = GetLastWrittenPageLSN();
		elog(DEBUG1, "zenith_get_request_lsn GetLastWrittenPageLSN lsn %X/%X ",
			(uint32) ((lsn) >> 32), (uint32) (lsn));

		if (lsn == InvalidXLogRecPtr)
		{
			/*
			 * We haven't evicted anything yet since the server was
			 * started. Then just use the latest flushed LSN. That's always
			 * safe, using the latest evicted LSN is really just an
			 * optimization.
			 */
			lsn = flushlsn;
			elog(DEBUG1, "zenith_get_request_lsn GetFlushRecPtr lsn %X/%X",
				 (uint32) ((lsn) >> 32), (uint32) (lsn));
		}
		else
			lsn = zm_adjust_lsn(lsn);

		/*
		 * Is it possible that the last-written LSN is ahead of last flush LSN? Probably not,
		 * we shouldn't evict a page from the buffer cache before all its modifications have
		 * been safely flushed. That's the "WAL before data" rule. But better safe than sorry.
		 */
		if (lsn > flushlsn)
		{
			elog(LOG, "last-written LSN %X/%X is ahead of last flushed LSN %X/%X",
				 (uint32) (lsn >> 32), (uint32) lsn,
				 (uint32) (flushlsn >> 32), (uint32) flushlsn);
			XLogFlush(lsn);
		}
	}
	return lsn;
}


/*
 *	zenith_exists() -- Does the physical file exist?
 */
bool
zenith_exists(SMgrRelation reln, ForkNumber forkNum)
{
	bool		ok;
	ZenithResponse *resp;

	resp = page_server->request((ZenithRequest) {
		.tag = T_ZenithExistsRequest,
		.page_key = {
			.rnode = reln->smgr_rnode.node,
			.forknum = forkNum
		},
		.lsn = zenith_get_request_lsn(false)
	});
	ok = resp->ok;
	pfree(resp);
	return ok;
}

/*
 *	zenith_create() -- Create a new relation on zenithd storage
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
void
zenith_create(SMgrRelation reln, ForkNumber forkNum, bool isRedo)
{
	elog(SmgrTrace, "Create relation %u/%u/%u.%u",
		 reln->smgr_rnode.node.spcNode,
		 reln->smgr_rnode.node.dbNode,
		 reln->smgr_rnode.node.relNode,
		 forkNum);

#ifdef DEBUG_COMPARE_LOCAL
	mdcreate(reln, forkNum, isRedo);
#endif
}

/*
 *	zenith_unlink() -- Unlink a relation.
 *
 * Note that we're passed a RelFileNodeBackend --- by the time this is called,
 * there won't be an SMgrRelation hashtable entry anymore.
 *
 * forkNum can be a fork number to delete a specific fork, or InvalidForkNumber
 * to delete all forks.
 *
 *
 * If isRedo is true, it's unsurprising for the relation to be already gone.
 * Also, we should remove the file immediately instead of queuing a request
 * for later, since during redo there's no possibility of creating a
 * conflicting relation.
 *
 * Note: any failure should be reported as WARNING not ERROR, because
 * we are usually not in a transaction anymore when this is called.
 */
void
zenith_unlink(RelFileNodeBackend rnode, ForkNumber forkNum, bool isRedo)
{
#ifdef DEBUG_COMPARE_LOCAL
	mdunlink(rnode, forkNum, isRedo);
#endif
}

/*
 *	zenith_extend() -- Add a block to the specified relation.
 *
 *		The semantics are nearly the same as mdwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
zenith_extend(SMgrRelation reln, ForkNumber forkNum, BlockNumber blkno,
			  char *buffer, bool skipFsync)
{
	XLogRecPtr lsn;

	zenith_wallog_page(reln, forkNum, blkno, buffer);

	lsn = PageGetLSN(buffer);
	elog(SmgrTrace, "smgrextend called for %u/%u/%u.%u blk %u, page LSN: %X/%08X",
		 reln->smgr_rnode.node.spcNode,
		 reln->smgr_rnode.node.dbNode,
		 reln->smgr_rnode.node.relNode,
		 forkNum, blkno,
		 (uint32) (lsn >> 32), (uint32) lsn);

#ifdef DEBUG_COMPARE_LOCAL
	mdextend(reln, forkNum, blkno, buffer, skipFsync);
#endif
}

/*
 *  zenith_open() -- Initialize newly-opened relation.
 */
void
zenith_open(SMgrRelation reln)
{
	/* no work */
	elog(SmgrTrace, "[ZENITH_SMGR] open noop");

#ifdef DEBUG_COMPARE_LOCAL
	mdopen(reln);
#endif
}

/*
 *	zenith_close() -- Close the specified relation, if it isn't closed already.
 */
void
zenith_close(SMgrRelation reln, ForkNumber forknum)
{
	/* no work */
	elog(SmgrTrace, "[ZENITH_SMGR] close noop");

#ifdef DEBUG_COMPARE_LOCAL
	mdclose(reln, forknum);
#endif
}

/*
 *	zenith_prefetch() -- Initiate asynchronous read of the specified block of a relation
 */
bool
zenith_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	/* not implemented */
	elog(SmgrTrace, "[ZENITH_SMGR] prefetch noop");
	return true;
}

/*
 * zenith_writeback() -- Tell the kernel to write pages back to storage.
 *
 * This accepts a range of blocks because flushing several pages at once is
 * considerably more efficient than doing so individually.
 */
void
zenith_writeback(SMgrRelation reln, ForkNumber forknum,
					  BlockNumber blocknum, BlockNumber nblocks)
{
	/* not implemented */
	elog(SmgrTrace, "[ZENITH_SMGR] writeback noop");

#ifdef DEBUG_COMPARE_LOCAL
	mdwriteback(reln, forknum, blocknum, nblocks);
#endif
}

/*
 *	zenith_read() -- Read the specified block from a relation.
 */
void
zenith_read(SMgrRelation reln, ForkNumber forkNum, BlockNumber blkno,
				 char *buffer)
{
	ZenithResponse *resp;
	XLogRecPtr request_lsn;

	request_lsn = zenith_get_request_lsn(false);
	resp = page_server->request((ZenithRequest) {
		.tag = T_ZenithReadRequest,
		.page_key = {
			.rnode = reln->smgr_rnode.node,
			.forknum = forkNum,
			.blkno = blkno
		},
		.lsn = request_lsn
	});

	if (!resp->ok)
		ereport(ERROR,
			(errcode(ERRCODE_IO_ERROR),
			errmsg("could not read block %u in rel %u/%u/%u.%u from page server at lsn %X/%08X",
					blkno,
					reln->smgr_rnode.node.spcNode,
					reln->smgr_rnode.node.dbNode,
					reln->smgr_rnode.node.relNode,
					forkNum,
					(uint32) (request_lsn >> 32), (uint32) request_lsn)));

	memcpy(buffer, resp->page, BLCKSZ);
	((PageHeader)buffer)->pd_flags &= ~PD_WAL_LOGGED; /* Clear PD_WAL_LOGGED bit stored in WAL record */
	pfree(resp);


#ifdef DEBUG_COMPARE_LOCAL
	if (forkNum == MAIN_FORKNUM)
	{
		char pageserver_masked[BLCKSZ];
		char mdbuf[BLCKSZ];
		char mdbuf_masked[BLCKSZ];

		mdread(reln, forkNum, blkno, mdbuf);

		memcpy(pageserver_masked, buffer, BLCKSZ);
		memcpy(mdbuf_masked, mdbuf, BLCKSZ);

		if (PageIsNew(mdbuf)) {
			if (!PageIsNew(pageserver_masked)) {
				elog(PANIC, "page is new in MD but not in Page Server at blk %u in rel %u/%u/%u fork %u (request LSN %X/%08X):\n%s\n",
					 blkno,
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 forkNum,
					 (uint32) (request_lsn >> 32), (uint32) request_lsn,
					 hexdump_page(buffer));
			}
		}
		else if (PageIsNew(buffer)) {
			elog(PANIC, "page is new in Page Server but not in MD at blk %u in rel %u/%u/%u fork %u (request LSN %X/%08X):\n%s\n",
					 blkno,
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 forkNum,
					 (uint32) (request_lsn >> 32), (uint32) request_lsn,
					 hexdump_page(mdbuf));
		}
		else if (PageGetSpecialSize(mdbuf) == 0)
		{
			// assume heap
			RmgrTable[RM_HEAP_ID].rm_mask(mdbuf_masked, blkno);
			RmgrTable[RM_HEAP_ID].rm_mask(pageserver_masked, blkno);

			if (memcmp(mdbuf_masked, pageserver_masked, BLCKSZ) != 0) {
				elog(PANIC, "heap buffers differ at blk %u in rel %u/%u/%u fork %u (request LSN %X/%08X):\n------ MD ------\n%s\n------ Page Server ------\n%s\n",
					 blkno,
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 forkNum,
					 (uint32) (request_lsn >> 32), (uint32) request_lsn,
					 hexdump_page(mdbuf_masked),
					 hexdump_page(pageserver_masked));
			}
		}
		else if (PageGetSpecialSize(mdbuf) == MAXALIGN(sizeof(BTPageOpaqueData)))
		{
			if (((BTPageOpaqueData *) PageGetSpecialPointer(mdbuf))->btpo_cycleid < MAX_BT_CYCLE_ID)
			{
				// assume btree
				RmgrTable[RM_BTREE_ID].rm_mask(mdbuf_masked, blkno);
				RmgrTable[RM_BTREE_ID].rm_mask(pageserver_masked, blkno);

				if (memcmp(mdbuf_masked, pageserver_masked, BLCKSZ) != 0) {
					elog(PANIC, "btree buffers differ at blk %u in rel %u/%u/%u fork %u (request LSN %X/%08X):\n------ MD ------\n%s\n------ Page Server ------\n%s\n",
						 blkno,
						 reln->smgr_rnode.node.spcNode,
						 reln->smgr_rnode.node.dbNode,
						 reln->smgr_rnode.node.relNode,
						 forkNum,
						 (uint32) (request_lsn >> 32), (uint32) request_lsn,
						 hexdump_page(mdbuf_masked),
						 hexdump_page(pageserver_masked));
				}
			}
		}
	}
#endif
}

#ifdef DEBUG_COMPARE_LOCAL
static char *
hexdump_page(char *page)
{
	StringInfoData result;

	initStringInfo(&result);

	for (int i = 0; i < BLCKSZ; i++)
	{
		if (i % 8 == 0)
			appendStringInfo(&result, " ");
		if (i % 40 == 0)
			appendStringInfo(&result, "\n");
		appendStringInfo(&result, "%02x", (unsigned char)(page[i]));
	}

	return result.data;
}
#endif


bool
zenith_nonrel_page_exists(RelFileNode rnode, BlockNumber blkno, int forknum)
{
	bool ok;
	ZenithResponse *resp;

	elog(SmgrTrace, "[ZENITH_SMGR] zenith_nonrel_page_exists relnode %u/%u/%u_%d blkno %u",
		rnode.spcNode, rnode.dbNode, rnode.relNode, forknum, blkno);

	resp = page_server->request((ZenithRequest) {
		.tag = T_ZenithExistsRequest,
		.page_key = {
			.rnode = rnode,
			.forknum = forknum,
			.blkno = blkno
		},
		.lsn = zenith_get_request_lsn(true)
	});
	ok = resp->ok;
	pfree(resp);
	return ok;
}

void
zenith_read_nonrel(RelFileNode rnode, BlockNumber blkno, char *buffer, int forknum)
{
	int bufsize = BLCKSZ;
	ZenithResponse *resp;
	XLogRecPtr lsn;

	//43 is magic for RELMAPPER_FILENAME in page cache
	// relmapper files has non-standard size of 512bytes
	if (forknum == 43)
		bufsize = 512;

	lsn = zenith_get_request_lsn(true);

	elog(SmgrTrace, "[ZENITH_SMGR] read nonrel relnode %u/%u/%u_%d blkno %u lsn %X/%X",
		rnode.spcNode, rnode.dbNode, rnode.relNode, forknum, blkno,
		(uint32) ((lsn) >> 32), (uint32) (lsn));

	resp = page_server->request((ZenithRequest) {
		.tag = T_ZenithReadRequest,
		.page_key = {
			.rnode = rnode,
			.forknum = forknum,
			.blkno = blkno
		},
		.lsn = lsn
	});

	if (!resp->ok)
		elog(ERROR, "[ZENITH_SMGR] smgr page not found");

	memcpy(buffer, resp->page, bufsize);
	pfree(resp);
}


/*
 *	zenith_write() -- Write the supplied block at the appropriate location.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use mdextend().
 */
void
zenith_write(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 char *buffer, bool skipFsync)
{
	XLogRecPtr lsn;

	zenith_wallog_page(reln, forknum, blocknum, buffer);

	lsn = PageGetLSN(buffer);
	elog(SmgrTrace, "smgrwrite called for %u/%u/%u.%u blk %u, page LSN: %X/%08X",
		 reln->smgr_rnode.node.spcNode,
		 reln->smgr_rnode.node.dbNode,
		 reln->smgr_rnode.node.relNode,
		 forknum, blocknum,
		 (uint32) (lsn >> 32), (uint32) lsn);

#ifdef DEBUG_COMPARE_LOCAL
	mdwrite(reln, forknum, blocknum, buffer, skipFsync);
#endif
}

/*
 *	zenith_nblocks() -- Get the number of blocks stored in a relation.
 */
BlockNumber
zenith_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	ZenithResponse *resp;
	int			n_blocks;
	XLogRecPtr request_lsn;

	request_lsn = zenith_get_request_lsn(false);
	resp = page_server->request((ZenithRequest) {
		.tag = T_ZenithNblocksRequest,
		.page_key = {
			.rnode = reln->smgr_rnode.node,
			.forknum = forknum,
		},
		.lsn = request_lsn
	});
	n_blocks = resp->n_blocks;

	elog(SmgrTrace, "zenith_nblocks: rel %u/%u/%u fork %u (request LSN %X/%08X): %u blocks",
		 reln->smgr_rnode.node.spcNode,
		 reln->smgr_rnode.node.dbNode,
		 reln->smgr_rnode.node.relNode,
		 forknum,
		 (uint32) (request_lsn >> 32), (uint32) request_lsn,
		 n_blocks);

	pfree(resp);
	return n_blocks;
}

/*
 *	zenith_truncate() -- Truncate relation to specified number of blocks.
 */
void
zenith_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
	XLogRecPtr lsn;

	/*
	 * Truncating a relation drops all its buffers from the buffer cache without
	 * calling smgrwrite() on them. But we must account for that in our tracking
	 * of last-written-LSN all the same: any future smgrnblocks() request must
	 * return the new size after the truncation. We don't know what the LSN of
	 * the truncation record was, so be conservative and use the most recently
	 * inserted WAL record's LSN.
	 */
	lsn = GetXLogInsertRecPtr();

	lsn = zm_adjust_lsn(lsn);

	/*
	 * Flush it, too. We don't actually care about it here, but let's uphold
	 * the invariant that last-written LSN <= flush LSN.
	 */
	XLogFlush(lsn);

	SetLastWrittenPageLSN(lsn);

#ifdef DEBUG_COMPARE_LOCAL
	mdtruncate(reln, forknum, nblocks);
#endif
}

/*
 *	zenith_immedsync() -- Immediately sync a relation to stable storage.
 *
 * Note that only writes already issued are synced; this routine knows
 * nothing of dirty buffers that may exist inside the buffer manager.  We
 * sync active and inactive segments; smgrDoPendingSyncs() relies on this.
 * Consider a relation skipping WAL.  Suppose a checkpoint syncs blocks of
 * some segment, then mdtruncate() renders that segment inactive.  If we
 * crash before the next checkpoint syncs the newly-inactive segment, that
 * segment may survive recovery, reintroducing unwanted data into the table.
 */
void
zenith_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	elog(SmgrTrace, "[ZENITH_SMGR] immedsync noop");

#ifdef DEBUG_COMPARE_LOCAL
	mdimmedsync(reln, forknum);
#endif
}

static const struct f_smgr zenith_smgr =
{
	.smgr_init = zenith_init,
	.smgr_shutdown = NULL,
	.smgr_open = zenith_open,
	.smgr_close = zenith_close,
	.smgr_create = zenith_create,
	.smgr_exists = zenith_exists,
	.smgr_unlink = zenith_unlink,
	.smgr_extend = zenith_extend,
	.smgr_prefetch = zenith_prefetch,
	.smgr_read = zenith_read,
	.smgr_write = zenith_write,
	.smgr_writeback = zenith_writeback,
	.smgr_nblocks = zenith_nblocks,
	.smgr_truncate = zenith_truncate,
	.smgr_immedsync = zenith_immedsync,
};


const f_smgr *
smgr_zenith(BackendId backend, RelFileNode rnode)
{

	/* Don't use page server for temp relations */
	if (backend != InvalidBackendId)
		return smgr_standard(backend, rnode);
	else
		return &zenith_smgr;
}

void
smgr_init_zenith(void)
{
	zenith_init();
}
