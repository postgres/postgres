
#include "smgr/pg_tde_smgr.h"
#include "postgres.h"
#include "storage/smgr.h"
#include "storage/md.h"
#include "catalog/catalog.h"
#include "encryption/enc_aes.h"
#include "access/pg_tde_tdemap.h"
#include "pg_tde_event_capture.h"

#ifdef PERCONA_EXT

typedef struct TDESMgrRelationData
{
	/* parent data */
	SMgrRelationData reln;
	/*
	 * for md.c; per-fork arrays of the number of open segments
	 * (md_num_open_segs) and the segments themselves (md_seg_fds).
	 */
	int md_num_open_segs[MAX_FORKNUM + 1];
	struct _MdfdVec *md_seg_fds[MAX_FORKNUM + 1];

	bool encrypted_relation;
	RelKeyData relKey;
} TDESMgrRelationData;

typedef TDESMgrRelationData *TDESMgrRelation;

/*
 * we only encrypt main and init forks
 */
static inline bool
tde_is_encryption_required(TDESMgrRelation tdereln, ForkNumber forknum)
{
	return (tdereln->encrypted_relation && (forknum == MAIN_FORKNUM || forknum == INIT_FORKNUM));
}

static RelKeyData*
tde_smgr_get_key(SMgrRelation reln)
{
	TdeCreateEvent *event;
	RelKeyData *rkd;
	TDEPrincipalKey *pk;

	if(IsCatalogRelationOid(reln->smgr_rlocator.locator.relNumber))
	{
		// do not try to encrypt/decrypt catalog tables
		return NULL;
	}

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
	pk = GetPrincipalKey(reln->smgr_rlocator.locator.dbOid, reln->smgr_rlocator.locator.spcOid, LW_SHARED);
	LWLockRelease(tde_lwlock_enc_keys());
	if(pk == NULL)
	{
		return NULL;
	}

	event = GetCurrentTdeCreateEvent();

	// see if we have a key for the relation, and return if yes
	rkd = GetSMGRRelationKey(reln->smgr_rlocator.locator);

	if(rkd != NULL)
	{
		return rkd;
	}

	// if this is a CREATE TABLE, we have to generate the key
	if(event->encryptMode == true && event->eventType == TDE_TABLE_CREATE_EVENT)
	{
		return pg_tde_create_smgr_key(&reln->smgr_rlocator.locator);
	}
	
	// if this is a CREATE INDEX, we have to load the key based on the table
	if(event->encryptMode == true && event->eventType == TDE_INDEX_CREATE_EVENT)
	{
		// For now keep it simple and create separate key for indexes
		// Later we might modify the map infrastructure to support the same keys
		return pg_tde_create_smgr_key(&reln->smgr_rlocator.locator);
	}

	return NULL;
}

static void
tde_mdwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	TDESMgrRelation tdereln = (TDESMgrRelation)reln;
	RelKeyData *rkd = &tdereln->relKey;

	if (!tde_is_encryption_required(tdereln, forknum))
	{
		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);
	}
	else
	{
		char *local_blocks = palloc(BLCKSZ * (nblocks + 1));
		char *local_blocks_aligned = (char *)TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);
		const void **local_buffers = palloc(sizeof(void *) * nblocks);

		AesInit();

		for(int i = 0; i  < nblocks; ++i )
		{
			int out_len = BLCKSZ;
			local_buffers[i] = &local_blocks_aligned[i * BLCKSZ];

			BlockNumber bn = blocknum + i;
			unsigned char iv[16] = {0,};
			memcpy(iv+4, &bn, sizeof(BlockNumber));

			AesEncrypt(rkd->internal_key.key, iv, ((char**)buffers)[i], BLCKSZ, local_buffers[i], &out_len);
		}

		mdwritev(reln, forknum, blocknum,
			local_buffers, nblocks, skipFsync);

		pfree(local_blocks);
		pfree(local_buffers);
	}
}

static void
tde_mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	TDESMgrRelation tdereln = (TDESMgrRelation)reln;
	RelKeyData *rkd = &tdereln->relKey;

	if (!tde_is_encryption_required(tdereln, forknum))
	{
		mdextend(reln, forknum, blocknum, buffer, skipFsync);
	}
	else
	{
		char *local_blocks = palloc(BLCKSZ * (1 + 1));
		char *local_blocks_aligned = (char *)TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);
		int out_len = BLCKSZ;
		unsigned char iv[16] = {
			0,
		};

		AesInit();
		memcpy(iv + 4, &blocknum, sizeof(BlockNumber));

		AesEncrypt(rkd->internal_key.key, iv, ((char *)buffer), BLCKSZ, local_blocks_aligned, &out_len);

		mdextend(reln, forknum, blocknum, local_blocks_aligned, skipFsync);

		pfree(local_blocks);
	}
}

static void
tde_mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		void **buffers, BlockNumber nblocks)
{
	int out_len = BLCKSZ;
	TDESMgrRelation tdereln = (TDESMgrRelation)reln;
	RelKeyData *rkd = &tdereln->relKey;

	mdreadv(reln, forknum, blocknum, buffers, nblocks);

	if (!tde_is_encryption_required(tdereln, forknum))
		return;

	AesInit();

	for(int i = 0; i < nblocks; ++i)
	{
		bool allZero = true;
		BlockNumber bn = blocknum + i;
		unsigned char iv[16] = {0,};

		for(int j = 0; j  < 32; ++j)
		{
			if(((char**)buffers)[i][j] != 0)
			{
				// Postgres creates all zero blocks in an optimized route, which we do not try
				// to encrypt.
				// Instead we detect if a block is all zero at decryption time, and
				// leave it as is.
				// This could be a security issue later, but it is a good first prototype
				allZero = false;
				break;
			}
		}
		if(allZero)
			continue;

		memcpy(iv+4, &bn, sizeof(BlockNumber));

		AesDecrypt(rkd->internal_key.key, iv, ((char **)buffers)[i], BLCKSZ, ((char **)buffers)[i], &out_len);
	}
}

static void
tde_mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	TDESMgrRelation tdereln = (TDESMgrRelation)reln;
	// This is the only function that gets called during actual CREATE TABLE/INDEX (EVENT TRIGGER)
	// so we create the key here by loading it
	// Later calls then decide to encrypt or not based on the existence of the key
	RelKeyData* key = tde_smgr_get_key(reln);
	if (key)
	{
		tdereln->encrypted_relation = true;
		memcpy(&tdereln->relKey, key, sizeof(RelKeyData));
	}
	else
	{
		tdereln->encrypted_relation = false;
	}

	return mdcreate(reln, forknum, isRedo);
}

/*
 * mdopen() -- Initialize newly-opened relation.
 */
static void
tde_mdopen(SMgrRelation reln)
{
	TDESMgrRelation tdereln = (TDESMgrRelation)reln;
	RelKeyData *key = tde_smgr_get_key(reln);
	if (key)
	{
		tdereln->encrypted_relation = true;
		memcpy(&tdereln->relKey, key, sizeof(RelKeyData));
	}
	else
	{
		tdereln->encrypted_relation = false;
	}
	mdopen(reln);
}

static SMgrId tde_smgr_id;
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

void RegisterStorageMgr(void)
{
	tde_smgr_id = smgr_register(&tde_smgr, sizeof(TDESMgrRelationData));

	// TODO: figure out how this part should work in a real extension
	storage_manager_id = tde_smgr_id; 
}

#else
void RegisterStorageMgr(void)
{
}
#endif /* PERCONA_EXT */
