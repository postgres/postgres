/*-------------------------------------------------------------------------
 *
 * pagestore_client.h
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * contrib/zenith/pagestore_client.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef pageserver_h
#define pageserver_h

#include "postgres.h"

#include "access/xlogdefs.h"
#include "storage/relfilenode.h"
#include "storage/block.h"
#include "storage/smgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/memutils.h"

#include "pg_config.h"

typedef enum
{
	/* pagestore_client -> pagestore */
	T_ZenithExistsRequest = 0,
	T_ZenithNblocksRequest,
	T_ZenithReadRequest,

	/* pagestore -> pagestore_client */
	T_ZenithStatusResponse = 100,
	T_ZenithNblocksResponse,
	T_ZenithReadResponse,
}			ZenithMessageTag;


/* base struct for c-style inheritance */
typedef struct
{
	ZenithMessageTag tag;
}			ZenithMessage;

#define messageTag(m)		(((const ZenithMessage *)(m))->tag)

extern char const *const ZenithMessageStr[];

typedef struct
{
	RelFileNode rnode;
	ForkNumber	forknum;
	BlockNumber blkno;
}			PageKey;

typedef struct
{
	ZenithMessageTag tag;
	uint64		system_id;
	PageKey		page_key;
	XLogRecPtr	lsn;			/* request page version @ this LSN */
}			ZenithRequest;

typedef struct
{
	ZenithMessageTag tag;
	bool		ok;
	uint32		n_blocks;
	char		page[1];
}			ZenithResponse;

StringInfoData zm_pack(ZenithMessage * msg);
ZenithMessage *zm_unpack(StringInfo s);
char	   *zm_to_string(ZenithMessage * msg);

/*
 * API
 */

typedef struct
{
	ZenithResponse *(*request) (ZenithRequest request);
}			page_server_api;

extern page_server_api * page_server;

extern char *page_server_connstring;
extern char *callmemaybe_connstring;
extern char *zenith_timeline;
extern bool wal_redo;

extern const f_smgr *smgr_zenith(BackendId backend, RelFileNode rnode);
extern void smgr_init_zenith(void);

extern const f_smgr *smgr_inmem(BackendId backend, RelFileNode rnode);
extern void smgr_init_inmem(void);
extern void smgr_shutdown_inmem(void);

/* zenith storage manager functionality */

extern void zenith_init(void);
extern void zenith_open(SMgrRelation reln);
extern void zenith_close(SMgrRelation reln, ForkNumber forknum);
extern void zenith_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool zenith_exists(SMgrRelation reln, ForkNumber forknum);
extern void zenith_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void zenith_extend(SMgrRelation reln, ForkNumber forknum,
						  BlockNumber blocknum, char *buffer, bool skipFsync);
extern bool zenith_prefetch(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum);
extern void zenith_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
						char *buffer);
extern void zenith_write(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, char *buffer, bool skipFsync);
extern void zenith_writeback(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber zenith_nblocks(SMgrRelation reln, ForkNumber forknum);
extern void zenith_truncate(SMgrRelation reln, ForkNumber forknum,
							BlockNumber nblocks);
extern void zenith_immedsync(SMgrRelation reln, ForkNumber forknum);

extern bool zenith_nonrel_page_exists(RelFileNode rnode, BlockNumber blkno, int forknum);
extern void zenith_read_nonrel(RelFileNode rnode, BlockNumber blkno, char *buffer, int forknum);

/* zenith wal-redo storage manager functionality */

extern void inmem_init(void);
extern void inmem_open(SMgrRelation reln);
extern void inmem_close(SMgrRelation reln, ForkNumber forknum);
extern void inmem_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool inmem_exists(SMgrRelation reln, ForkNumber forknum);
extern void inmem_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void inmem_extend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, char *buffer, bool skipFsync);
extern bool inmem_prefetch(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum);
extern void inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					   char *buffer);
extern void inmem_write(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, char *buffer, bool skipFsync);
extern void inmem_writeback(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber inmem_nblocks(SMgrRelation reln, ForkNumber forknum);
extern void inmem_truncate(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber nblocks);
extern void inmem_immedsync(SMgrRelation reln, ForkNumber forknum);

#endif
