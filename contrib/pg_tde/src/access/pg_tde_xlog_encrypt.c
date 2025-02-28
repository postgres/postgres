/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog_encrypt.c
 *	  Encrypted XLog storage manager
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_xlog_encrypt.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_tde.h"
#include "pg_tde_defines.h"
#include "pg_tde_guc.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog_encrypt.h"
#include "catalog/tde_global_space.h"
#include "encryption/enc_tde.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#else
#include "port/atomics.h"
#endif

static const XLogSmgr tde_xlog_smgr = {
	.seg_read = tdeheap_xlog_seg_read,
	.seg_write = tdeheap_xlog_seg_write,
};

static void SetXLogPageIVPrefix(TimeLineID tli, XLogRecPtr lsn, char *iv_prefix);

#ifndef FRONTEND
static Size TDEXLogEncryptBuffSize(void);
static ssize_t TDEXLogWriteEncryptedPages(int fd, const void *buf, size_t count,
										  off_t offset, TimeLineID tli,
										  XLogSegNo segno);

typedef struct EncryptionStateData
{
	char	   *segBuf;
	char		db_keydata_path[MAXPGPATH];
	pg_atomic_uint64 enc_key_lsn;	/* to sync with readers */
} EncryptionStateData;

static EncryptionStateData *EncryptionState = NULL;

/* TODO: can be swapped out to the disk */
static InternalKey EncryptionKey =
{
	.rel_type = MAP_ENTRY_EMPTY,
	.start_lsn = InvalidXLogRecPtr,
	.ctx = NULL,
};

static int	XLOGChooseNumBuffers(void);

/*  This can't be a GUC check hook, because that would run too soon during startup */
void
TDEXlogCheckSane(void)
{
	if (EncryptXLog)
	{
		InternalKey *key = GetRelationKey(GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID), TDE_KEY_TYPE_GLOBAL, true);

		if (key == NULL)
		{
			ereport(ERROR,
					(errmsg("WAL encryption can only be enabled with a properly configured principal key. Disable pg_tde.wal_encrypt and create one using pg_tde_set_server_principal_key() or pg_tde_set_global_principal_key() before enabling it.")));
		}
	}
}

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
	return (Size) XLOG_BLCKSZ * xbuffers;
}

Size
TDEXLogEncryptStateSize(void)
{
	Size		sz;

	sz = TYPEALIGN(PG_IO_ALIGN_SIZE, TDEXLogEncryptBuffSize());
	sz = add_size(sz, sizeof(EncryptionStateData));

	return MAXALIGN(sz);
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

	/*
	 * TODO: we need enc_key_lsn all the time but encrypt buffer only when
	 * EncryptXLog is on
	 */
	EncryptionState = (EncryptionStateData *)
		ShmemInitStruct("TDE XLog Encryption State",
						TDEXLogEncryptStateSize(),
						&foundBuf);

	allocptr = ((char *) EncryptionState) + TYPEALIGN(PG_IO_ALIGN_SIZE, sizeof(EncryptionStateData));
	EncryptionState->segBuf = allocptr;

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
	char		iv_prefix[16] = {0,};
	InternalKey *key = &EncryptionKey;
	char	   *enc_buff = EncryptionState->segBuf;

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "write encrypted WAL, size: %lu, offset: %ld [%lX], seg: %X/%X, key_start_lsn: %X/%X",
		 count, offset, offset, LSN_FORMAT_ARGS(segno), LSN_FORMAT_ARGS(key->start_lsn));
#endif

	SetXLogPageIVPrefix(tli, segno, iv_prefix);
	PG_TDE_ENCRYPT_DATA(iv_prefix, offset,
						(char *) buf, count,
						enc_buff, key);

	return pg_pwrite(fd, enc_buff, count, offset);
}

#endif							/* !FRONTEND */

void
TDEXLogSmgrInit(void)
{
#ifndef FRONTEND
	/* TODO: move to the separate func, it's not an SMGR init */
	InternalKey *key = pg_tde_read_last_wal_key();

	/* TDOO: clean-up this mess */
	if ((!key && EncryptXLog) || (key &&
								  ((key->rel_type & TDE_KEY_TYPE_WAL_ENCRYPTED && !EncryptXLog) ||
								   (key->rel_type & TDE_KEY_TYPE_WAL_UNENCRYPTED && EncryptXLog))))
	{
		pg_tde_create_wal_key(
							  &EncryptionKey, &GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID),
							  (EncryptXLog ? TDE_KEY_TYPE_WAL_ENCRYPTED : TDE_KEY_TYPE_WAL_UNENCRYPTED));
	}
	else if (key)
	{
		EncryptionKey = *key;
		pfree(key);
		pg_atomic_write_u64(&EncryptionState->enc_key_lsn, EncryptionKey.start_lsn);
	}

	pg_tde_set_db_file_paths(GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID).dbOid, NULL, EncryptionState->db_keydata_path);

#endif
	SetXLogSmgr(&tde_xlog_smgr);
}

ssize_t
tdeheap_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset,
					   TimeLineID tli, XLogSegNo segno)
{
#ifndef FRONTEND

	/*
	 * Set the last (most recent) key's start LSN if not set.
	 *
	 * This func called with WALWriteLock held, so no need in any extra sync.
	 */
	if (EncryptionKey.rel_type & TDE_KEY_TYPE_GLOBAL &&
		pg_atomic_read_u64(&EncryptionState->enc_key_lsn) == 0)
	{
		XLogRecPtr	lsn;

		XLogSegNoOffsetToRecPtr(segno, offset, wal_segment_size, lsn);

		pg_tde_wal_last_key_set_lsn(lsn, EncryptionState->db_keydata_path);
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
ssize_t
tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset,
					  TimeLineID tli, XLogSegNo segno, int segSize)
{
	ssize_t		readsz;
	char		iv_prefix[16] = {0,};
	WALKeyCacheRec *keys = pg_tde_get_wal_cache_keys();
	XLogRecPtr	write_key_lsn = 0;
	WALKeyCacheRec *curr_key = NULL;
	off_t		dec_off = 0;
	off_t		dec_end = 0;
	size_t		dec_sz = 0;
	XLogRecPtr	data_start;
	XLogRecPtr	data_end;

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "read from a WAL segment, size: %lu offset: %ld [%lX], seg: %X/%X",
		 count, offset, offset, LSN_FORMAT_ARGS(segno));
#endif

	/*
	 * Read data from disk
	 */
	readsz = pg_pread(fd, buf, count, offset);

	if (!keys)
	{
		/* cache is empty, try to read keys from disk */
		keys = pg_tde_fetch_wal_keys(0);
	}

#ifndef FRONTEND
	write_key_lsn = pg_atomic_read_u64(&EncryptionState->enc_key_lsn);
#endif

	if (write_key_lsn != 0)
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

	SetXLogPageIVPrefix(tli, segno, iv_prefix);

	XLogSegNoOffsetToRecPtr(segno, offset, segSize, data_start);
	XLogSegNoOffsetToRecPtr(segno, offset + count, segSize, data_end);

	/*
	 * TODO: this is higly ineffective. We should get rid of linked list and
	 * search from the last key as this is what the walsender is useing.
	 */
	curr_key = keys;
	while (curr_key)
	{
#ifdef TDE_XLOG_DEBUG
		elog(DEBUG1, "WAL key %X/%X-%X/%X, encrypted: %s",
			 LSN_FORMAT_ARGS(curr_key->start_lsn),
			 LSN_FORMAT_ARGS(curr_key->end_lsn),
			 curr_key->key->rel_type & TDE_KEY_TYPE_WAL_ENCRYPTED ? "yes" : "no");
#endif

		if (curr_key->key->start_lsn != InvalidXLogRecPtr &&
			(curr_key->key->rel_type & TDE_KEY_TYPE_WAL_ENCRYPTED))
		{
			/*
			 * Check if the key's range overlaps with the buffer's and decypt
			 * the part that does.
			 */
			if (data_start <= curr_key->end_lsn && curr_key->start_lsn <= data_end)
			{
				dec_off = XLogSegmentOffset(Max(data_start, curr_key->start_lsn), segSize);
				dec_end = XLogSegmentOffset(Min(data_end, curr_key->end_lsn), segSize);

				/* We have reached the end of the segment */
				if (dec_end == 0)
				{
					dec_end = offset + count;
				}

				dec_sz = dec_end - dec_off;

#ifdef TDE_XLOG_DEBUG
				elog(DEBUG1, "decrypt WAL, dec_off: %lu [buff_off %lu], sz: %lu | key %X/%X",
					 dec_off, offset - dec_off, dec_sz, LSN_FORMAT_ARGS(curr_key->key->start_lsn));
#endif
				PG_TDE_DECRYPT_DATA(iv_prefix, dec_off,
									(char *) buf + (offset - dec_off),
									dec_sz, (char *) buf + (offset - dec_off),
									curr_key->key);

				if (dec_off + dec_sz == offset)
				{
					break;
				}
			}
		}

		curr_key = curr_key->next;
	}

	return readsz;
}

/* IV: TLI(uint32) + XLogRecPtr(uint64)*/
static inline void
SetXLogPageIVPrefix(TimeLineID tli, XLogRecPtr lsn, char *iv_prefix)
{
	iv_prefix[0] = (tli >> 24);
	iv_prefix[1] = ((tli >> 16) & 0xFF);
	iv_prefix[2] = ((tli >> 8) & 0xFF);
	iv_prefix[3] = (tli & 0xFF);

	iv_prefix[4] = (lsn >> 56);
	iv_prefix[5] = ((lsn >> 48) & 0xFF);
	iv_prefix[6] = ((lsn >> 40) & 0xFF);
	iv_prefix[7] = ((lsn >> 32) & 0xFF);
	iv_prefix[8] = ((lsn >> 24) & 0xFF);
	iv_prefix[9] = ((lsn >> 16) & 0xFF);
	iv_prefix[10] = ((lsn >> 8) & 0xFF);
	iv_prefix[11] = (lsn & 0xFF);
}
