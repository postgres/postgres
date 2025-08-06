#include "postgres.h"

#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"
#include "pg_tde_event_capture.h"
#include "smgr/pg_tde_smgr.h"

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

typedef struct
{
	RelFileLocator rel;
	InternalKey key;
} TempRelKeyEntry;

#define INIT_TEMP_RELS 16

/*
 * Each backend has a hashtable that stores the keys for all temproary tables.
 */
static HTAB *TempRelKeys = NULL;

static SMgrId OurSMgrId = MaxSMgrId;

static void tde_smgr_save_temp_key(const RelFileLocator *newrlocator, const InternalKey *key);
static InternalKey *tde_smgr_get_temp_key(const RelFileLocator *rel);
static bool tde_smgr_has_temp_key(const RelFileLocator *rel);
static void tde_smgr_remove_temp_key(const RelFileLocator *rel);
static void CalcBlockIv(ForkNumber forknum, BlockNumber bn, const unsigned char *base_iv, unsigned char *iv);

static void
tde_smgr_log_create_key(const RelFileLocator *rlocator)
{
	XLogRelKey	xlrec = {.rlocator = *rlocator};

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_RELATION_KEY);
}

static void
tde_smgr_log_delete_key(const RelFileLocator *rlocator)
{
	XLogRelKey	xlrec = {.rlocator = *rlocator};

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_DELETE_RELATION_KEY);
}

static InternalKey *
tde_smgr_create_key(const RelFileLocatorBackend *smgr_rlocator)
{
	InternalKey *key = palloc_object(InternalKey);

	pg_tde_generate_internal_key(key);

	if (RelFileLocatorBackendIsTemp(*smgr_rlocator))
		tde_smgr_save_temp_key(&smgr_rlocator->locator, key);
	else
	{
		pg_tde_save_smgr_key(smgr_rlocator->locator, key);
		tde_smgr_log_create_key(&smgr_rlocator->locator);
	}

	return key;
}

void
tde_smgr_create_key_redo(const RelFileLocator *rlocator)
{
	InternalKey key;

	if (pg_tde_has_smgr_key(*rlocator))
		return;

	pg_tde_generate_internal_key(&key);

	pg_tde_save_smgr_key(*rlocator, &key);
}

static void
tde_smgr_remove_key(const RelFileLocatorBackend *smgr_rlocator)
{
	if (RelFileLocatorBackendIsTemp(*smgr_rlocator))
		tde_smgr_remove_temp_key(&smgr_rlocator->locator);
	else
		pg_tde_free_key_map_entry(smgr_rlocator->locator);
}

static void
tde_smgr_delete_key(const RelFileLocatorBackend *smgr_rlocator)
{
	if (!RelFileLocatorBackendIsTemp(*smgr_rlocator))
	{
		pg_tde_free_key_map_entry(smgr_rlocator->locator);
		tde_smgr_log_delete_key(&smgr_rlocator->locator);
	}
}

void
tde_smgr_delete_key_redo(const RelFileLocator *rlocator)
{
	pg_tde_free_key_map_entry(*rlocator);
}

static bool
tde_smgr_is_encrypted(const RelFileLocatorBackend *smgr_rlocator)
{
	if (RelFileLocatorBackendIsTemp(*smgr_rlocator))
		return tde_smgr_has_temp_key(&smgr_rlocator->locator);
	else
		return pg_tde_has_smgr_key(smgr_rlocator->locator);
}

static InternalKey *
tde_smgr_get_key(const RelFileLocatorBackend *smgr_rlocator)
{
	if (RelFileLocatorBackendIsTemp(*smgr_rlocator))
		return tde_smgr_get_temp_key(&smgr_rlocator->locator);
	else
		return pg_tde_get_smgr_key(smgr_rlocator->locator);
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

				return tde_smgr_is_encrypted(&old_smgr_locator);
			}
	}

	return false;
}

bool
tde_smgr_rel_is_encrypted(SMgrRelation reln)
{
	TDESMgrRelation *tdereln = (TDESMgrRelation *) reln;

	if (reln->smgr_which != OurSMgrId)
		return false;

	return tdereln->encryption_status == RELATION_KEY_AVAILABLE ||
		tdereln->encryption_status == RELATION_KEY_NOT_AVAILABLE;
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
		if (tde_smgr_is_encrypted(&rlocator))
			tde_smgr_remove_key(&rlocator);
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
	InternalKey *key;

	mdcreate(relold, reln, forknum, isRedo);

	/*
	 * Creating the key is handled by a separate WAL record on redo and
	 * fetching the key can be delayed to when we actually need it like we do
	 * for other forks anyway.
	 */
	if (isRedo)
		return;

	/*
	 * Only create keys when creating the main fork. Other forks are created
	 * later and use the key which was created when creating the main fork.
	 */
	if (forknum != MAIN_FORKNUM)
		return;

	/*
	 * If we have a key for this relation already, we need to remove it. This
	 * can happen if OID is re-used after a crash left a key for a
	 * non-existing relation in the key file.
	 *
	 * If we're in redo, a separate WAL record will make sure the key is
	 * removed.
	 */
	tde_smgr_delete_key(&reln->smgr_rlocator);

	if (!tde_smgr_should_encrypt(&reln->smgr_rlocator, &relold))
	{
		tdereln->encryption_status = RELATION_NOT_ENCRYPTED;
		return;
	}

	key = tde_smgr_create_key(&reln->smgr_rlocator);

	tdereln->encryption_status = RELATION_KEY_AVAILABLE;
	tdereln->relKey = *key;
	pfree(key);
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
	OurSMgrId = smgr_register(&tde_smgr, sizeof(TDESMgrRelation));
	storage_manager_id = OurSMgrId;
}

static void
tde_smgr_save_temp_key(const RelFileLocator *newrlocator, const InternalKey *key)
{
	TempRelKeyEntry *entry;
	bool		found;

	if (TempRelKeys == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(RelFileLocator);
		ctl.entrysize = sizeof(TempRelKeyEntry);
		TempRelKeys = hash_create("pg_tde temporary relation keys",
								  INIT_TEMP_RELS,
								  &ctl,
								  HASH_ELEM | HASH_BLOBS);
	}

	entry = (TempRelKeyEntry *) hash_search(TempRelKeys,
											newrlocator,
											HASH_ENTER, &found);
	Assert(!found);

	entry->key = *key;
}

static InternalKey *
tde_smgr_get_temp_key(const RelFileLocator *rel)
{
	TempRelKeyEntry *entry;

	if (TempRelKeys == NULL)
		return NULL;

	entry = hash_search(TempRelKeys, rel, HASH_FIND, NULL);

	if (entry)
	{
		InternalKey *key = palloc_object(InternalKey);

		*key = entry->key;
		return key;
	}

	return NULL;
}

static bool
tde_smgr_has_temp_key(const RelFileLocator *rel)
{
	return TempRelKeys && hash_search(TempRelKeys, rel, HASH_FIND, NULL);
}

static void
tde_smgr_remove_temp_key(const RelFileLocator *rel)
{
	Assert(TempRelKeys);
	hash_search(TempRelKeys, rel, HASH_REMOVE, NULL);
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
