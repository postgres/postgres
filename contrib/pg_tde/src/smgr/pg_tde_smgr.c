
#include "smgr/pg_tde_smgr.h"
#include "postgres.h"
#include "storage/smgr.h"
#include "storage/md.h"
#include "catalog/catalog.h"
#include "encryption/enc_aes.h"
#include "access/pg_tde_tdemap.h"
#include "pg_tde_event_capture.h"

typedef struct TDESMgrRelationData
{
	/* parent data */
	SMgrRelationData reln;

	/*
	 * for md.c; per-fork arrays of the number of open segments
	 * (md_num_open_segs) and the segments themselves (md_seg_fds).
	 */
	int			md_num_open_segs[MAX_FORKNUM + 1];
	struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];

	bool		encrypted_relation;
	InternalKey relKey;
} TDESMgrRelationData;

typedef TDESMgrRelationData *TDESMgrRelation;

static void CalcBlockIv(BlockNumber bn, const unsigned char *base_iv, unsigned char *iv);

static InternalKey *
tde_smgr_get_key(SMgrRelation reln, RelFileLocator *old_locator, bool can_create)
{
	TdeCreateEvent *event;
	InternalKey *key;

	if (IsCatalogRelationOid(reln->smgr_rlocator.locator.relNumber))
	{
		/* do not try to encrypt/decrypt catalog tables */
		return NULL;
	}

	/* see if we have a key for the relation, and return if yes */
	key = GetSMGRRelationKey(reln->smgr_rlocator);
	if (key != NULL)
	{
		return key;
	}

	event = GetCurrentTdeCreateEvent();

	/*
	 * Can be many things, such as: CREATE TABLE ALTER TABLE SET ACCESS METHOD
	 * ALTER TABLE something else on an encrypted table CREATE INDEX ...
	 *
	 * Every file has its own key, that makes logistics easier.
	 */
	if (event->encryptMode == true && can_create)
	{
		return pg_tde_create_smgr_key(&reln->smgr_rlocator);
	}

	/* check if we had a key for the old locator, if there's one */
	if (old_locator != NULL && can_create)
	{
		RelFileLocatorBackend rlocator = {.locator = *old_locator,.backend = reln->smgr_rlocator.backend};
		InternalKey *oldkey = GetSMGRRelationKey(rlocator);

		if (oldkey != NULL)
		{
			/* create a new key for the new file */
			return pg_tde_create_smgr_key(&reln->smgr_rlocator);
		}
	}

	return NULL;
}

static void
tde_mdwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	TDESMgrRelation tdereln = (TDESMgrRelation) reln;
	InternalKey *int_key = &tdereln->relKey;

	if (!tdereln->encrypted_relation)
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
	}
	else
	{
		unsigned char *local_blocks = palloc(BLCKSZ * (nblocks + 1));
		unsigned char *local_blocks_aligned = (unsigned char *) TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);
		void	  **local_buffers = palloc_array(void *, nblocks);

		AesInit();

		for (int i = 0; i < nblocks; ++i)
		{
			BlockNumber bn = blocknum + i;
			unsigned char iv[16];

			local_buffers[i] = &local_blocks_aligned[i * BLCKSZ];

			CalcBlockIv(bn, int_key->base_iv, iv);

			AesEncrypt(int_key->key, iv, ((unsigned char **) buffers)[i], BLCKSZ, local_buffers[i]);
		}

		mdwritev(reln, forknum, blocknum,
				 (const void **) local_buffers, nblocks, skipFsync);

		pfree(local_blocks);
		pfree(local_buffers);
	}
}

static void
tde_mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 const void *buffer, bool skipFsync)
{
	TDESMgrRelation tdereln = (TDESMgrRelation) reln;
	InternalKey *int_key = &tdereln->relKey;

	if (!tdereln->encrypted_relation)
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
	}
	else
	{
		unsigned char *local_blocks = palloc(BLCKSZ * (1 + 1));
		unsigned char *local_blocks_aligned = (unsigned char *) TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);
		unsigned char iv[16];

		AesInit();

		CalcBlockIv(blocknum, int_key->base_iv, iv);

		AesEncrypt(int_key->key, iv, ((unsigned char *) buffer), BLCKSZ, local_blocks_aligned);

		mdextend(reln, forknum, blocknum, local_blocks_aligned, skipFsync);

		pfree(local_blocks);
	}
}

static void
tde_mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			void **buffers, BlockNumber nblocks)
{
	TDESMgrRelation tdereln = (TDESMgrRelation) reln;
	InternalKey *int_key = &tdereln->relKey;

	mdreadv(reln, forknum, blocknum, buffers, nblocks);

	if (!tdereln->encrypted_relation)
		return;

	AesInit();

	for (int i = 0; i < nblocks; ++i)
	{
		bool		allZero = true;
		BlockNumber bn = blocknum + i;
		unsigned char iv[16];

		/*
		 * Detect unencrypted all-zero pages written by smgrzeroextend() by
		 * looking at the first 32 bytes of the page.
		 *
		 * Not encrypting all-zero pages is safe because they are only written
		 * at the end of the file when extending a table on disk so they tend
		 * to be short lived plus they only leak a slightly more accurate
		 * table size than one can glean from just the file size.
		 */
		for (int j = 0; j < 32; ++j)
		{
			if (((char **) buffers)[i][j] != 0)
			{
				allZero = false;
				break;
			}
		}
		if (allZero)
			continue;

		CalcBlockIv(bn, int_key->base_iv, iv);

		AesDecrypt(int_key->key, iv, ((unsigned char **) buffers)[i], BLCKSZ, ((unsigned char **) buffers)[i]);
	}
}

static void
tde_mdcreate(RelFileLocator relold, SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	TDESMgrRelation tdereln = (TDESMgrRelation) reln;
	InternalKey *key;
	TdeCreateEvent *event = GetCurrentTdeCreateEvent();

	/*
	 * Make sure that even if a statement failed, and an event trigger end
	 * trigger didn't fire, we don't accidentaly create encrypted files when
	 * we don't have to. event above is a pointer, so it will reflect the
	 * correct state even if this changes it.
	 */
	validateCurrentEventTriggerState(false);

	/*
	 * This is the only function that gets called during actual CREATE
	 * TABLE/INDEX (EVENT TRIGGER)
	 */
	/* so we create the key here by loading it */

	mdcreate(relold, reln, forknum, isRedo);

	if (forknum == MAIN_FORKNUM || forknum == INIT_FORKNUM)
	{
		/*
		 * Only create keys when creating the main/init fork. Other forks can
		 * be created later, even during tde creation events. We definitely do
		 * not want to create keys then, even later, when we encrypt all
		 * forks!
		 */

		/*
		 * Later calls then decide to encrypt or not based on the existence of
		 * the key
		 */
		key = tde_smgr_get_key(reln, event->alterAccessMethodMode ? NULL : &relold, true);

		if (key)
		{
			tdereln->encrypted_relation = true;
			tdereln->relKey = *key;
		}
		else
		{
			tdereln->encrypted_relation = false;
		}
	}
}

/*
 * mdopen() -- Initialize newly-opened relation.
 */
static void
tde_mdopen(SMgrRelation reln)
{
	TDESMgrRelation tdereln = (TDESMgrRelation) reln;
	InternalKey *key = tde_smgr_get_key(reln, NULL, false);

	if (key)
	{
		tdereln->encrypted_relation = true;
		tdereln->relKey = *key;
	}
	else
	{
		tdereln->encrypted_relation = false;
	}
	mdopen(reln);
}

static const struct f_smgr tde_smgr = {
	.name = "tde",
	.smgr_init = mdinit,
	.smgr_shutdown = NULL,
	.smgr_open = tde_mdopen,
	.smgr_close = mdclose,
	.smgr_create = tde_mdcreate,
	.smgr_exists = mdexists,
	.smgr_unlink = mdunlink,
	.smgr_extend = tde_mdextend,
	.smgr_zeroextend = mdzeroextend,
	.smgr_prefetch = mdprefetch,
	.smgr_readv = tde_mdreadv,
	.smgr_writev = tde_mdwritev,
	.smgr_writeback = mdwriteback,
	.smgr_nblocks = mdnblocks,
	.smgr_truncate = mdtruncate,
	.smgr_immedsync = mdimmedsync,
	.smgr_registersync = mdregistersync,
};

void
RegisterStorageMgr(void)
{
	if (storage_manager_id != MdSMgrId)
		elog(FATAL, "Another storage manager was loaded before pg_tde. Multiple storage managers is unsupported.");
	storage_manager_id = smgr_register(&tde_smgr, sizeof(TDESMgrRelationData));
}

/*
 * The intialization vector of a block is its block number conmverted to a
 * 128 bit big endian number XOR the base IV of the relation file.
 */
static void
CalcBlockIv(BlockNumber bn, const unsigned char *base_iv, unsigned char *iv)
{
	memset(iv, 0, 16);

	iv[12] = bn >> 24;
	iv[13] = bn >> 16;
	iv[14] = bn >> 8;
	iv[15] = bn;

	for (int i = 0; i < 16; i++)
		iv[i] ^= base_iv[i];
}
