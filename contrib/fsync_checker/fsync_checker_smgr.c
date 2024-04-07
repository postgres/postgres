#include "postgres.h"

#include "access/xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/md.h"
#include "utils/hsearch.h"

PG_MODULE_MAGIC;

typedef struct volatileRelnKey
{
	RelFileLocator locator;
	ForkNumber	forknum;
}			volatileRelnKey;

typedef struct volatileRelnEntry
{
	volatileRelnKey key;
	XLogRecPtr	lsn;
}			volatileRelnEntry;

void		_PG_init(void);

static void fsync_checker_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
								 const void *buffer, bool skipFsync);
static void fsync_checker_immedsync(SMgrRelation reln, ForkNumber forknum);
static void fsync_checker_writev(SMgrRelation reln, ForkNumber forknum,
								 BlockNumber blocknum, const void **buffers,
								 BlockNumber nblocks, bool skipFsync);
static void fsync_checker_writeback(SMgrRelation reln, ForkNumber forknum,
									BlockNumber blocknum, BlockNumber nblocks);
static void fsync_checker_zeroextend(SMgrRelation reln, ForkNumber forknum,
									 BlockNumber blocknum, int nblocks, bool skipFsync);

static void fsync_checker_checkpoint_create(const CheckPoint *checkPoint);
static void fsync_checker_shmem_request(void);
static void fsync_checker_shmem_startup(void);

static void add_reln(SMgrRelation reln, ForkNumber forknum);
static void remove_reln(SMgrRelation reln, ForkNumber forknum);

static SMgrId fsync_checker_smgr_id;
static const struct f_smgr fsync_checker_smgr = {
	.name = "fsync_checker",
	.smgr_init = mdinit,
	.smgr_shutdown = NULL,
	.smgr_open = mdopen,
	.smgr_close = mdclose,
	.smgr_create = mdcreate,
	.smgr_exists = mdexists,
	.smgr_unlink = mdunlink,
	.smgr_extend = fsync_checker_extend,
	.smgr_zeroextend = fsync_checker_zeroextend,
	.smgr_prefetch = mdprefetch,
	.smgr_readv = mdreadv,
	.smgr_writev = fsync_checker_writev,
	.smgr_writeback = fsync_checker_writeback,
	.smgr_nblocks = mdnblocks,
	.smgr_truncate = mdtruncate,
	.smgr_immedsync = fsync_checker_immedsync,
	.smgr_registersync = mdregistersync,
};

static HTAB *volatile_relns;
static LWLock *volatile_relns_lock;
static shmem_request_hook_type prev_shmem_request_hook;
static shmem_startup_hook_type prev_shmem_startup_hook;
static checkpoint_create_hook_type prev_checkpoint_create_hook;

void
_PG_init(void)
{
	prev_checkpoint_create_hook = checkpoint_create_hook;
	checkpoint_create_hook = fsync_checker_checkpoint_create;

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = fsync_checker_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = fsync_checker_shmem_startup;

	/*
	 * Relation size of 0 means we can just defer to md, but it would be nice
	 * to just expose this functionality, so if I needed my own relation, I
	 * could use MdSmgrRelation as the parent.
	 */
	fsync_checker_smgr_id = smgr_register(&fsync_checker_smgr, 0);

	storage_manager_id = fsync_checker_smgr_id;
}

static void
fsync_checker_checkpoint_create(const CheckPoint *checkPoint)
{
	long		num_entries;
	HASH_SEQ_STATUS status;
	volatileRelnEntry *entry;

	if (prev_checkpoint_create_hook)
		prev_checkpoint_create_hook(checkPoint);

	LWLockAcquire(volatile_relns_lock, LW_EXCLUSIVE);

	hash_seq_init(&status, volatile_relns);

	num_entries = hash_get_num_entries(volatile_relns);
	elog(INFO, "Analyzing %ld volatile relations", num_entries);
	while ((entry = hash_seq_search(&status)))
	{
		if (entry->lsn < checkPoint->redo)
		{
			char	   *path;

			path = relpathperm(entry->key.locator, entry->key.forknum);

			elog(WARNING, "Relation not previously synced: %s", path);

			pfree(path);
		}
	}

	LWLockRelease(volatile_relns_lock);
}

static void
fsync_checker_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(hash_estimate_size(1024, sizeof(volatileRelnEntry)));
	RequestNamedLWLockTranche("fsync_checker volatile relns lock", 1);
}

static void
fsync_checker_shmem_startup(void)
{
	HASHCTL		ctl;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	ctl.keysize = sizeof(volatileRelnKey);
	ctl.entrysize = sizeof(volatileRelnEntry);
	volatile_relns = NULL;
	volatile_relns_lock = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	volatile_relns = ShmemInitHash("fsync_checker volatile relns",
								   1024, 1024, &ctl, HASH_BLOBS | HASH_ELEM);
	volatile_relns_lock = &GetNamedLWLockTranche("fsync_checker volatile relns lock")->lock;

	LWLockRelease(AddinShmemInitLock);
}

static void
add_reln(SMgrRelation reln, ForkNumber forknum)
{
	bool		found;
	XLogRecPtr	lsn;
	volatileRelnKey key;
	volatileRelnEntry *entry;

	key.locator = reln->smgr_rlocator.locator;
	key.forknum = forknum;

	lsn = GetXLogWriteRecPtr();

	LWLockAcquire(volatile_relns_lock, LW_EXCLUSIVE);

	entry = hash_search(volatile_relns, &key, HASH_ENTER, &found);
	if (!found)
		entry->lsn = lsn;

	LWLockRelease(volatile_relns_lock);
}

static void
remove_reln(SMgrRelation reln, ForkNumber forknum)
{
	volatileRelnKey key;

	key.locator = reln->smgr_rlocator.locator;
	key.forknum = forknum;

	LWLockAcquire(volatile_relns_lock, LW_EXCLUSIVE);

	hash_search(volatile_relns, &key, HASH_REMOVE, NULL);

	LWLockRelease(volatile_relns_lock);
}

static void
fsync_checker_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					 const void *buffer, bool skipFsync)
{
	if (!SmgrIsTemp(reln) && !skipFsync)
		add_reln(reln, forknum);

	mdextend(reln, forknum, blocknum, buffer, skipFsync);
}

static void
fsync_checker_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	if (!SmgrIsTemp(reln))
		remove_reln(reln, forknum);

	mdimmedsync(reln, forknum);
}

static void
fsync_checker_writev(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, const void **buffers,
					 BlockNumber nblocks, bool skipFsync)
{
	if (!SmgrIsTemp(reln) && !skipFsync)
		add_reln(reln, forknum);

	mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
}

static void
fsync_checker_writeback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks)
{
	if (!SmgrIsTemp(reln))
		remove_reln(reln, forknum);

	mdwriteback(reln, forknum, blocknum, nblocks);
}

static void
fsync_checker_zeroextend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	if (!SmgrIsTemp(reln) && !skipFsync)
		add_reln(reln, forknum);

	mdzeroextend(reln, forknum, blocknum, nblocks, skipFsync);
}
