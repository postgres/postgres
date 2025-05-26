/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog_smgr.c
 *	  Encrypted XLog storage manager
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_xlog_smgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_tde.h"
#include "pg_tde_defines.h"
#include "pg_tde_guc.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlog_smgr.h"
#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog_smgr.h"
#include "catalog/tde_global_space.h"
#include "encryption/enc_tde.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#else
#include "port/atomics.h"
#endif

static void CalcXLogPageIVPrefix(TimeLineID tli, XLogRecPtr lsn, const unsigned char *base_iv, char *iv_prefix);
static ssize_t tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset,
									 TimeLineID tli, XLogSegNo segno, int segSize);
static ssize_t tdeheap_xlog_seg_write(int fd, const void *buf, size_t count,
									  off_t offset, TimeLineID tli,
									  XLogSegNo segno);

static const XLogSmgr tde_xlog_smgr = {
	.seg_read = tdeheap_xlog_seg_read,
	.seg_write = tdeheap_xlog_seg_write,
};

#ifndef FRONTEND
static Size TDEXLogEncryptBuffSize(void);

/*
 * Must be the same as in replication/walsender.c
 *
 * This is used to calculate the encryption buffer size.
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

static ssize_t TDEXLogWriteEncryptedPages(int fd, const void *buf, size_t count,
										  off_t offset, TimeLineID tli,
										  XLogSegNo segno);

typedef struct EncryptionStateData
{
	char	   *segBuf;
	char		db_map_path[MAXPGPATH];
	pg_atomic_uint64 enc_key_lsn;	/* to sync with readers */
} EncryptionStateData;

static EncryptionStateData *EncryptionState = NULL;

/* TODO: can be swapped out to the disk */
static InternalKey EncryptionKey =
{
	.type = MAP_ENTRY_EMPTY,
	.start_lsn = InvalidXLogRecPtr,
};
static void *EncryptionCryptCtx = NULL;

static int	XLOGChooseNumBuffers(void);

static int
XLOGChooseNumBuffers(void)
{
	int			xbuffers;

	xbuffers = NBuffers / 32;
	if (xbuffers > (wal_segment_size / XLOG_BLCKSZ))
		xbuffers = (wal_segment_size / XLOG_BLCKSZ);
	if (xbuffers < 8)
		xbuffers = 8;
	return xbuffers;
}

/*
 * Defines the size of the XLog encryption buffer
 */
static Size
TDEXLogEncryptBuffSize(void)
{
	int			xbuffers;

	xbuffers = (XLOGbuffers == -1) ? XLOGChooseNumBuffers() : XLOGbuffers;
	return Max(MAX_SEND_SIZE, mul_size(XLOG_BLCKSZ, xbuffers));
}

Size
TDEXLogEncryptStateSize(void)
{
	Size		sz;

	sz = sizeof(EncryptionStateData);
	if (EncryptXLog)
	{
		sz = add_size(sz, TDEXLogEncryptBuffSize());
		sz = add_size(sz, PG_IO_ALIGN_SIZE);
	}

	return sz;
}

/*
 * Alloc memory for the encryption buffer.
 *
 * It should fit XLog buffers (XLOG_BLCKSZ * wal_buffers). We can't
 * (re)alloc this buf in tdeheap_xlog_seg_write() based on the write size as
 * it's called in the CRIT section, hence no allocations are allowed.
 *
 * Access to this buffer happens during XLogWrite() call which should
 * be called with WALWriteLock held, hence no need in extra locks.
 */
void
TDEXLogShmemInit(void)
{
	bool		foundBuf;
	char	   *allocptr;

	EncryptionState = (EncryptionStateData *)
		ShmemInitStruct("TDE XLog Encryption State",
						TDEXLogEncryptStateSize(),
						&foundBuf);

	memset(EncryptionState, 0, sizeof(EncryptionStateData));

	if (EncryptXLog)
	{
		allocptr = ((char *) EncryptionState) + sizeof(EncryptionStateData);
		allocptr = (char *) TYPEALIGN(PG_IO_ALIGN_SIZE, allocptr);
		EncryptionState->segBuf = allocptr;

		Assert((char *) EncryptionState + TDEXLogEncryptStateSize() >= (char *) EncryptionState->segBuf + TDEXLogEncryptBuffSize());
	}

	pg_atomic_init_u64(&EncryptionState->enc_key_lsn, 0);

	elog(DEBUG1, "pg_tde: initialized encryption buffer %lu bytes", TDEXLogEncryptStateSize());
}

/*
 * Encrypt XLog page(s) from the buf and write to the segment file.
 */
static ssize_t
TDEXLogWriteEncryptedPages(int fd, const void *buf, size_t count, off_t offset,
						   TimeLineID tli, XLogSegNo segno)
{
	char		iv_prefix[16];
	InternalKey *key = &EncryptionKey;
	char	   *enc_buff = EncryptionState->segBuf;

	Assert(count <= TDEXLogEncryptBuffSize());

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "write encrypted WAL, size: %lu, offset: %ld [%lX], seg: %X/%X, key_start_lsn: %X/%X",
		 count, offset, offset, LSN_FORMAT_ARGS(segno), LSN_FORMAT_ARGS(key->start_lsn));
#endif

	CalcXLogPageIVPrefix(tli, segno, key->base_iv, iv_prefix);
	pg_tde_stream_crypt(iv_prefix, offset,
						(char *) buf, count,
						enc_buff, key, &EncryptionCryptCtx);

	return pg_pwrite(fd, enc_buff, count, offset);
}

#endif							/* !FRONTEND */

void
TDEXLogSmgrInit(void)
{
#ifndef FRONTEND
	/* TODO: move to the separate func, it's not an SMGR init */
	InternalKey *key = pg_tde_read_last_wal_key();

	/*
	 * Always generate a new key on starting PostgreSQL to protect against
	 * attacks on CTR ciphers based on comparing the WAL generated by two
	 * divergent copies of the same cluster.
	 */
	if (EncryptXLog)
	{
		pg_tde_create_wal_key(&EncryptionKey, &GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID),
							  TDE_KEY_TYPE_WAL_ENCRYPTED);
	}
	else if (key && key->type & TDE_KEY_TYPE_WAL_ENCRYPTED)
	{
		pg_tde_create_wal_key(&EncryptionKey, &GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID),
							  TDE_KEY_TYPE_WAL_UNENCRYPTED);
	}
	else if (key)
	{
		EncryptionKey = *key;
		pg_atomic_write_u64(&EncryptionState->enc_key_lsn, EncryptionKey.start_lsn);
	}

	if (key)
		pfree(key);

	pg_tde_set_db_file_path(GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID).dbOid, EncryptionState->db_map_path);

#endif
	SetXLogSmgr(&tde_xlog_smgr);
}

static ssize_t
tdeheap_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset,
					   TimeLineID tli, XLogSegNo segno)
{
#ifndef FRONTEND

	/*
	 * Set the last (most recent) key's start LSN if not set.
	 *
	 * This func called with WALWriteLock held, so no need in any extra sync.
	 */
	if (EncryptionKey.type & TDE_KEY_TYPE_GLOBAL &&
		pg_atomic_read_u64(&EncryptionState->enc_key_lsn) == 0)
	{
		XLogRecPtr	lsn;

		XLogSegNoOffsetToRecPtr(segno, offset, wal_segment_size, lsn);

		pg_tde_wal_last_key_set_lsn(lsn, EncryptionState->db_map_path);
		EncryptionKey.start_lsn = lsn;
		pg_atomic_write_u64(&EncryptionState->enc_key_lsn, lsn);
	}

	if (EncryptXLog)
		return TDEXLogWriteEncryptedPages(fd, buf, count, offset, tli, segno);
	else
#endif
		return pg_pwrite(fd, buf, count, offset);
}

/*
 * Read the XLog pages from the segment file and dectypt if need.
 */
static ssize_t
tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset,
					  TimeLineID tli, XLogSegNo segno, int segSize)
{
	ssize_t		readsz;
	WALKeyCacheRec *keys = pg_tde_get_wal_cache_keys();
	XLogRecPtr	write_key_lsn;
	XLogRecPtr	data_start;
	XLogRecPtr	data_end;

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "read from a WAL segment, size: %lu offset: %ld [%lX], seg: %X/%X",
		 count, offset, offset, LSN_FORMAT_ARGS(segno));
#endif

	readsz = pg_pread(fd, buf, count, offset);

	if (readsz <= 0)
		return readsz;

	if (!keys)
	{
		/* cache is empty, try to read keys from disk */
		keys = pg_tde_fetch_wal_keys(InvalidXLogRecPtr);
	}

#ifndef FRONTEND
	write_key_lsn = pg_atomic_read_u64(&EncryptionState->enc_key_lsn);

	if (!XLogRecPtrIsInvalid(write_key_lsn))
	{
		WALKeyCacheRec *last_key = pg_tde_get_last_wal_key();

		Assert(last_key);

		/* write has generated a new key, need to fetch it */
		if (last_key->start_lsn < write_key_lsn)
		{
			pg_tde_fetch_wal_keys(write_key_lsn);

			/* in case cache was empty before */
			keys = pg_tde_get_wal_cache_keys();
		}
	}
#endif

	XLogSegNoOffsetToRecPtr(segno, offset, segSize, data_start);
	XLogSegNoOffsetToRecPtr(segno, offset + readsz, segSize, data_end);

	/*
	 * TODO: this is higly ineffective. We should get rid of linked list and
	 * search from the last key as this is what the walsender is useing.
	 */
	for (WALKeyCacheRec *curr_key = keys; curr_key != NULL; curr_key = curr_key->next)
	{
#ifdef TDE_XLOG_DEBUG
		elog(DEBUG1, "WAL key %X/%X-%X/%X, encrypted: %s",
			 LSN_FORMAT_ARGS(curr_key->start_lsn),
			 LSN_FORMAT_ARGS(curr_key->end_lsn),
			 curr_key->key.type & TDE_KEY_TYPE_WAL_ENCRYPTED ? "yes" : "no");
#endif

		if (curr_key->key.start_lsn != InvalidXLogRecPtr &&
			(curr_key->key.type & TDE_KEY_TYPE_WAL_ENCRYPTED))
		{
			/*
			 * Check if the key's range overlaps with the buffer's and decypt
			 * the part that does.
			 */
			if (data_start < curr_key->end_lsn && data_end > curr_key->start_lsn)
			{
				char		iv_prefix[16];
				off_t		dec_off = XLogSegmentOffset(Max(data_start, curr_key->start_lsn), segSize);
				off_t		dec_end = XLogSegmentOffset(Min(data_end, curr_key->end_lsn), segSize);
				size_t		dec_sz;
				char	   *dec_buf = (char *) buf + (dec_off - offset);

				Assert(dec_off >= offset);

				CalcXLogPageIVPrefix(tli, segno, curr_key->key.base_iv, iv_prefix);

				/* We have reached the end of the segment */
				if (dec_end == 0)
				{
					dec_end = offset + readsz;
				}

				dec_sz = dec_end - dec_off;

#ifdef TDE_XLOG_DEBUG
				elog(DEBUG1, "decrypt WAL, dec_off: %lu [buff_off %lu], sz: %lu | key %X/%X",
					 dec_off, dec_off - offset, dec_sz, LSN_FORMAT_ARGS(curr_key->key->start_lsn));
#endif
				pg_tde_stream_crypt(iv_prefix, dec_off, dec_buf, dec_sz, dec_buf,
									&curr_key->key, &curr_key->crypt_ctx);

				if (dec_off + dec_sz == offset)
				{
					break;
				}
			}
		}
	}

	return readsz;
}

union u128cast
{
	char		a[16];
	unsigned	__int128 i;
};

/*
 * Calculate the start IV for an XLog segmenet.
 *
 * IV: (TLI(uint32) + XLogRecPtr(uint64)) + BaseIV(uint8[12])
 *
 * TODO: Make the calculation more like OpenSSL's CTR withot any gaps and
 * preferrably without zeroing the lowest bytes for the base IV.
 *
 * TODO: This code vectorizes poorly in both gcc and clang.
 */
static void
CalcXLogPageIVPrefix(TimeLineID tli, XLogRecPtr lsn, const unsigned char *base_iv, char *iv_prefix)
{
	union u128cast base;
	union u128cast iv;
	unsigned	__int128 offset;

	for (int i = 0; i < 16; i++)
#ifdef WORDS_BIGENDIAN
		base.a[i] = base_iv[i];
#else
		base.a[i] = base_iv[15 - i];
#endif

	/* We do not support wrapping addition in Aes128EncryptedZeroBlocks() */
	base.i &= ~(((unsigned __int128) 1) << 32);

	offset = (((unsigned __int128) tli) << 112) | (((unsigned __int128) lsn) << 32);

	iv.i = base.i + offset;

	for (int i = 0; i < 16; i++)
#ifdef WORDS_BIGENDIAN
		iv_prefix[i] = iv.a[i];
#else
		iv_prefix[i] = iv.a[15 - i];
#endif
}
