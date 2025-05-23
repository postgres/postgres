#include "smgr/pg_tde_smgr.h"
#include "postgres.h"
#include "storage/smgr.h"
#include "storage/md.h"
#include "catalog/catalog.h"
#include "encryption/enc_aes.h"
#include "access/pg_tde_tdemap.h"
#include "pg_tde_event_capture.h"

typedef enum TDEMgrRelationEncryptionStatus
{
	/* This is a plaintext relation */
	RELATION_NOT_ENCRYPTED = 0,

	/* This is an encrypted relation, and we have the key available. */
	RELATION_KEY_AVAILABLE = 1,

	/* This is an encrypted relation, but we haven't loaded the key yet. */
	RELATION_KEY_NOT_AVAILABLE = 2,
} TDEMgrRelationEncryptionStatus;

/*
 * TDESMgrRelation is an extended copy of MDSMgrRelationData in md.c
 *
 * The first fields of this struct must always exactly match
 * MDSMgrRelationData since we will pass this structure to the md.c functions.
 *
 * Any fields specific to the tde smgr must be placed after these fields.
 */
typedef struct TDESMgrRelation
{
	/* parent data */
	SMgrRelationData reln;

	/*
	 * for md.c; per-fork arrays of the number of open segments
	 * (md_num_open_segs) and the segments themselves (md_seg_fds).
	 */
	int			md_num_open_segs[MAX_FORKNUM + 1];
	struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];

	TDEMgrRelationEncryptionStatus encryption_status;
	InternalKey relKey;
} TDESMgrRelation;

static void CalcBlockIv(ForkNumber forknum, BlockNumber bn, const unsigned char *base_iv, unsigned char *iv);

static bool
tde_smgr_is_encrypted(const RelFileLocatorBackend *smgr_rlocator)
{
	/* Do not try to encrypt/decrypt catalog tables */
	if (IsCatalogRelationOid(smgr_rlocator->locator.relNumber))
		return false;

	return IsSMGRRelationEncrypted(*smgr_rlocator);
}

static InternalKey *
tde_smgr_get_key(const RelFileLocatorBackend *smgr_rlocator)
{
	/* Do not try to encrypt/decrypt catalog tables */
	if (IsCatalogRelationOid(smgr_rlocator->locator.relNumber))
		return NULL;

	return GetSMGRRelationKey(*smgr_rlocator);
}

static bool
tde_smgr_should_encrypt(const RelFileLocatorBackend *smgr_rlocator, RelFileLocator *old_locator)
{
	/* Do not try to encrypt/decrypt catalog tables */
	if (IsCatalogRelationOid(smgr_rlocator->locator.relNumber))
		return false;

	switch (currentTdeEncryptModeValidated())
	{
		case TDE_ENCRYPT_MODE_PLAIN:
			return false;
		case TDE_ENCRYPT_MODE_ENCRYPT:
			return true;
		case TDE_ENCRYPT_MODE_RETAIN:
			if (old_locator)
			{
				RelFileLocatorBackend old_smgr_locator = {
					.locator = *old_locator,
					.backend = smgr_rlocator->backend,
				};

				return IsSMGRRelationEncrypted(old_smgr_locator);
			}
	}

	return false;
}

static void
tde_mdwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	if (tdereln->encryption_status == RELATION_NOT_ENCRYPTED)
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
	}
	else
	{
		unsigned char *local_blocks = palloc_aligned(BLCKSZ * nblocks, PG_IO_ALIGN_SIZE, 0);
		void	  **local_buffers = palloc_array(void *, nblocks);

		if (tdereln->encryption_status == RELATION_KEY_NOT_AVAILABLE)
		{
			InternalKey *int_key = tde_smgr_get_key(&reln->smgr_rlocator);

			tdereln->relKey = *int_key;
			tdereln->encryption_status = RELATION_KEY_AVAILABLE;
			pfree(int_key);
		}

		for (int i = 0; i < nblocks; ++i)
		{
			BlockNumber bn = blocknum + i;
			unsigned char iv[16];

			local_buffers[i] = &local_blocks[i * BLCKSZ];

			CalcBlockIv(forknum, bn, tdereln->relKey.base_iv, iv);

			AesEncrypt(tdereln->relKey.key, iv, ((unsigned char **) buffers)[i], BLCKSZ, local_buffers[i]);
		}

		mdwritev(reln, forknum, blocknum,
				 (const void **) local_buffers, nblocks, skipFsync);

		pfree(local_blocks);
		pfree(local_buffers);
	}
}

/*
 * The current transaction might already be commited when this function is
 * called, so do not call any code that uses ereport(ERROR) or otherwise tries
 * to abort the transaction.
 */
static void
tde_mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	mdunlink(rlocator, forknum, isRedo);

	/*
	 * As of PostgreSQL 17 we are called once per forks, no matter if they
	 * exist or not, from smgrdounlinkall() so deleting the relation key on
	 * attempting to delete the main fork is safe. Additionally since we
	 * unlink the files after commit/abort we do not need to care about
	 * concurrent accesses.
	 *
	 * We support InvalidForkNumber to be similar to mdunlink() but it can
	 * actually never happen.
	 */
	if (forknum == MAIN_FORKNUM || forknum == InvalidForkNumber)
	{
		if (IsSMGRRelationEncrypted(rlocator))
			DeleteSMGRRelationKey(rlocator);
	}
}

static void
tde_mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 const void *buffer, bool skipFsync)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	if (tdereln->encryption_status == RELATION_NOT_ENCRYPTED)
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
	}
	else
	{
		unsigned char *local_blocks = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE, 0);
		unsigned char iv[16];

		if (tdereln->encryption_status == RELATION_KEY_NOT_AVAILABLE)
		{
			InternalKey *int_key = tde_smgr_get_key(&reln->smgr_rlocator);

			tdereln->relKey = *int_key;
			tdereln->encryption_status = RELATION_KEY_AVAILABLE;
			pfree(int_key);
		}

		CalcBlockIv(forknum, blocknum, tdereln->relKey.base_iv, iv);

		AesEncrypt(tdereln->relKey.key, iv, ((unsigned char *) buffer), BLCKSZ, local_blocks);

		mdextend(reln, forknum, blocknum, local_blocks, skipFsync);

		pfree(local_blocks);
	}
}

static void
tde_mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			void **buffers, BlockNumber nblocks)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	mdreadv(reln, forknum, blocknum, buffers, nblocks);

	if (tdereln->encryption_status == RELATION_NOT_ENCRYPTED)
		return;
	else if (tdereln->encryption_status == RELATION_KEY_NOT_AVAILABLE)
	{
		InternalKey *int_key = tde_smgr_get_key(&reln->smgr_rlocator);

		tdereln->relKey = *int_key;
		tdereln->encryption_status = RELATION_KEY_AVAILABLE;
		pfree(int_key);
	}

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

		CalcBlockIv(forknum, bn, tdereln->relKey.base_iv, iv);

		AesDecrypt(tdereln->relKey.key, iv, ((unsigned char **) buffers)[i], BLCKSZ, ((unsigned char **) buffers)[i]);
	}
}

static void
tde_mdcreate(RelFileLocator relold, SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	/* Copied from mdcreate() in md.c */
	if (isRedo && tdereln->md_num_open_segs[forknum] > 0)
		return;

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
		 *
		 * Later calls then decide to encrypt or not based on the existence of
		 * the key.
		 *
		 * Since event triggers do not fire on the standby or in recovery we
		 * do not try to generate any new keys and instead trust the xlog.
		 */
		InternalKey *key = tde_smgr_get_key(&reln->smgr_rlocator);

		if (!isRedo && !key && tde_smgr_should_encrypt(&reln->smgr_rlocator, &relold))
			key = pg_tde_create_smgr_key(&reln->smgr_rlocator);

		if (key)
		{
			tdereln->encryption_status = RELATION_KEY_AVAILABLE;
			tdereln->relKey = *key;
			pfree(key);
		}
		else
		{
			tdereln->encryption_status = RELATION_NOT_ENCRYPTED;
		}
	}
}

/*
 * mdopen() -- Initialize newly-opened relation.
 *
 * The current transaction might already be commited when this function is
 * called, so do not call any code that uses ereport(ERROR) or otherwise tries
 * to abort the transaction.
 */
static void
tde_mdopen(SMgrRelation reln)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	mdopen(reln);

	if (tde_smgr_is_encrypted(&reln->smgr_rlocator))
	{
		tdereln->encryption_status = RELATION_KEY_NOT_AVAILABLE;
	}
	else
	{
		tdereln->encryption_status = RELATION_NOT_ENCRYPTED;
	}
}

static const struct f_smgr tde_smgr = {
	.name = "tde",
	.smgr_init = mdinit,
	.smgr_shutdown = NULL,
	.smgr_open = tde_mdopen,
	.smgr_close = mdclose,
	.smgr_create = tde_mdcreate,
	.smgr_exists = mdexists,
	.smgr_unlink = tde_mdunlink,
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
	storage_manager_id = smgr_register(&tde_smgr, sizeof(TDESMgrRelation));
}

/*
 * The intialization vector of a block is its block number conmverted to a
 * 128 bit big endian number plus the forknumber XOR the base IV of the
 * relation file.
 */
static void
CalcBlockIv(ForkNumber forknum, BlockNumber bn, const unsigned char *base_iv, unsigned char *iv)
{
	memset(iv, 0, 16);

	/* The init fork is copied to the main fork so we must use the same IV */
	iv[7] = forknum == INIT_FORKNUM ? MAIN_FORKNUM : forknum;

	iv[12] = bn >> 24;
	iv[13] = bn >> 16;
	iv[14] = bn >> 8;
	iv[15] = bn;

	for (int i = 0; i < 16; i++)
		iv[i] ^= base_iv[i];
}
