/*-------------------------------------------------------------------------
 *
 * inmem_smgr.c
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/zenith/inmem_smgr.c
 *
 * TODO cleanup obsolete copy-pasted comments
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "pagestore_client.h"
#include "utils/hsearch.h"
#include "access/xlog.h"

typedef struct
{
	RelFileNode node;
	ForkNumber	forknum;
	BlockNumber blkno;
}			WrNodeKey;

typedef struct
{
	WrNodeKey	tag;
	char		data[BLCKSZ];
}			WrNode;

HTAB	   *inmem_files;

/*
 *	inmem_init() -- Initialize private state
 */
void
inmem_init(void)
{
	HASHCTL		hashCtl;

	hashCtl.keysize = sizeof(WrNodeKey);
	hashCtl.entrysize = sizeof(WrNode);

	if (inmem_files)
		hash_destroy(inmem_files);

	inmem_files = hash_create("wal-redo files map",
							  1024,
							  &hashCtl,
							  HASH_ELEM | HASH_BLOBS);
}

/*
 *	inmem_exists() -- Does the physical file exist?
 */
bool
inmem_exists(SMgrRelation reln, ForkNumber forknum)
{
	WrNodeKey	key;

	key.node = reln->smgr_rnode.node;
	key.forknum = forknum;
	key.blkno = 0;
	return hash_search(inmem_files,
					   &key,
					   HASH_FIND,
					   NULL) != NULL;
}

/*
 *	inmem_create() -- Create a new relation on zenithd storage
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
void
inmem_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
}

/*
 *	inmem_unlink() -- Unlink a relation.
 *
 * Note that we're passed a RelFileNodeBackend --- by the time this is called,
 * there won't be an SMgrRelation hashtable entry anymore.
 *
 * forknum can be a fork number to delete a specific fork, or InvalidForkNumber
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
inmem_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo)
{
}

/*
 *	inmem_extend() -- Add a block to the specified relation.
 *
 *		The semantics are nearly the same as mdwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
void
inmem_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 char *buffer, bool skipFsync)
{
	WrNodeKey	key;
	WrNode	   *node;

	key.node = reln->smgr_rnode.node;
	key.forknum = forknum;
	key.blkno = blkno;
	node = hash_search(inmem_files,
					   &key,
					   HASH_ENTER,
					   NULL);
	memcpy(node->data, buffer, BLCKSZ);
}

/*
 *  inmem_open() -- Initialize newly-opened relation.
 */
void
inmem_open(SMgrRelation reln)
{
}

/*
 *	inmem_close() -- Close the specified relation, if it isn't closed already.
 */
void
inmem_close(SMgrRelation reln, ForkNumber forknum)
{
}

/*
 *	inmem_prefetch() -- Initiate asynchronous read of the specified block of a relation
 */
bool
inmem_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return true;
}

/*
 * inmem_writeback() -- Tell the kernel to write pages back to storage.
 *
 * This accepts a range of blocks because flushing several pages at once is
 * considerably more efficient than doing so individually.
 */
void
inmem_writeback(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blocknum, BlockNumber nblocks)
{
}

/*
 *	inmem_read() -- Read the specified block from a relation.
 */
void
inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
		   char *buffer)
{
	WrNodeKey	key;
	WrNode	   *node;

	key.node = reln->smgr_rnode.node;
	key.forknum = forknum;
	key.blkno = blkno;
	node = hash_search(inmem_files,
					   &key,
					   HASH_FIND,
					   NULL);
	if (node != NULL)
		memcpy(buffer, node->data, BLCKSZ);
	else
		memset(buffer, 0, BLCKSZ);
}

/*
 *	inmem_write() -- Write the supplied block at the appropriate location.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use mdextend().
 */
void
inmem_write(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			char *buffer, bool skipFsync)
{
	WrNodeKey	key;
	WrNode	   *node;

	key.node = reln->smgr_rnode.node;
	key.forknum = forknum;
	key.blkno = blocknum;
	node = hash_search(inmem_files,
					   &key,
					   HASH_ENTER,
					   NULL);
	memcpy(node->data, buffer, BLCKSZ);
}

/*
 *	inmem_nblocks() -- Get the number of blocks stored in a relation.
 */
BlockNumber
inmem_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	WrNodeKey	key;
	WrNode	   *node;

	key.node = reln->smgr_rnode.node;
	key.forknum = forknum;
	key.blkno = 0;

	while (true)
	{
		node = hash_search(inmem_files,
						   &key,
						   HASH_FIND,
						   NULL);
		if (node == NULL)
			return key.blkno;
		key.blkno += 1;
	}
}

/*
 *	inmem_truncate() -- Truncate relation to specified number of blocks.
 */
void
inmem_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
}

/*
 *	inmem_immedsync() -- Immediately sync a relation to stable storage.
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
inmem_immedsync(SMgrRelation reln, ForkNumber forknum)
{
}
static const struct f_smgr inmem_smgr =
{
	.smgr_init = inmem_init,
	.smgr_shutdown = NULL,
	.smgr_open = inmem_open,
	.smgr_close = inmem_close,
	.smgr_create = inmem_create,
	.smgr_exists = inmem_exists,
	.smgr_unlink = inmem_unlink,
	.smgr_extend = inmem_extend,
	.smgr_prefetch = inmem_prefetch,
	.smgr_read = inmem_read,
	.smgr_write = inmem_write,
	.smgr_writeback = inmem_writeback,
	.smgr_nblocks = inmem_nblocks,
	.smgr_truncate = inmem_truncate,
	.smgr_immedsync = inmem_immedsync,
};

const f_smgr *
smgr_inmem(BackendId backend, RelFileNode rnode)
{
	if (backend != InvalidBackendId && !InRecovery)
		return smgr_standard(backend, rnode);
	else
	{
		return &inmem_smgr;
	}
}

void
smgr_init_inmem()
{
	inmem_init();
}
