
#include "smgr/pg_tde_smgr.h"
#include "postgres.h"
#include "storage/smgr.h"
#include "storage/md.h"
#include "catalog/catalog.h"
#include "encryption/enc_aes.h"

#if PG_VERSION_NUM >= 170000

// TODO: implement proper key/IV
static char key[16] = {0,};
// iv should be based on blocknum, available in the API
static char iv[16] = {0,};

void
tde_mdwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	AesInit();
	
	char* local_blocks = malloc( BLCKSZ * (nblocks+1) );
	char* local_blocks_aligned = (char*)TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);
	const void** local_buffers = malloc ( sizeof(void*) * nblocks );

	// TODO: add check to only encrypt/decrypt tables with specific AM/DB?

	if(IsCatalogRelationOid(reln->smgr_rlocator.locator.spcOid))
	{
		// Don't try to encrypt catalog tables:
		// Issues with bootstrap and encryption metadata

		mdwritev(reln, forknum, blocknum, buffers, nblocks, skipFsync);

		return;
	}

	for(int i = 0; i  < nblocks; ++i )
	{
		local_buffers[i] = &local_blocks_aligned[i*BLCKSZ];	
		int out_len = BLCKSZ;
		AesEncrypt(key, iv, ((char**)buffers)[i], BLCKSZ, local_buffers[i], &out_len);
	}

	mdwritev(reln, forknum, blocknum,
		 local_buffers, nblocks, skipFsync);

	free(local_blocks);
	free(local_buffers);
}

void
tde_mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	AesInit();
	
	char* local_blocks = malloc( BLCKSZ * (1+1) );
	char* local_blocks_aligned = (char*)TYPEALIGN(PG_IO_ALIGN_SIZE, local_blocks);

	// TODO: add check to only encrypt/decrypt tables with specific AM/DB?

	if(IsCatalogRelationOid(reln->smgr_rlocator.locator.spcOid))
	{
		// Don't try to encrypt catalog tables:
		// Issues with bootstrap and encryption metadata
		mdextend(reln, forknum, blocknum, buffer, skipFsync);

		return;
	}

	int out_len = BLCKSZ;
	AesEncrypt(key, iv, ((char*)buffer), BLCKSZ, local_blocks_aligned, &out_len);

	mdextend(reln, forknum, blocknum, local_blocks_aligned, skipFsync);

	
	free(local_blocks);
}

void
tde_mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		void **buffers, BlockNumber nblocks)
{
	AesInit();

	mdreadv(reln, forknum, blocknum, buffers, nblocks);

	// TODO: add check to only encrypt/decrypt tables with specific AM/DB?

	// Don't try to decrypt catalog tables, those are not encrypted
	if(IsCatalogRelationOid(reln->smgr_rlocator.locator.spcOid))
	{
		return;
	}

	for(int i = 0; i < nblocks; ++i)
	{
		bool allZero = true;
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
		if(allZero) continue;

		int out_len = BLCKSZ;
		AesDecrypt(key, iv, ((char**)buffers)[i], BLCKSZ, ((char**)buffers)[i], &out_len);
	}

	// And now decrypt buffers in place
	// We check the first few bytes of the page: if all zero, we assume it is zero and keep it as is
}
static SMgrId tde_smgr_id;
static const struct f_smgr tde_smgr = {
	.name = "tde",
	.smgr_init = mdinit,
	.smgr_shutdown = NULL,
	.smgr_open = mdopen,
	.smgr_close = mdclose,
	.smgr_create = mdcreate,
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
};

void RegisterStorageMgr()
{
    tde_smgr_id = smgr_register(&tde_smgr, 0);

	// TODO: figure out how this part should work in a real extension
	storage_manager_id = tde_smgr_id; 
}

#else
void RegisterStorageMgr()
{
}
#endif