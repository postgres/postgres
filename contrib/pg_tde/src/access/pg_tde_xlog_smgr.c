/*
 * Encrypted XLog storage manager
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlog_smgr.h"
#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "access/pg_tde_xlog_keys.h"
#include "access/pg_tde_xlog_smgr.h"
#include "catalog/tde_global_space.h"
#include "encryption/enc_tde.h"
#include "pg_tde.h"
#include "pg_tde_defines.h"

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
									  XLogSegNo segno, int segSize);

static const XLogSmgr tde_xlog_smgr = {
	.seg_read = tdeheap_xlog_seg_read,
	.seg_write = tdeheap_xlog_seg_write,
};

static void *EncryptionCryptCtx = NULL;

/* TODO: can be swapped out to the disk */
static WalEncryptionKey EncryptionKey =
{
	.type = WAL_KEY_TYPE_INVALID,
	.wal_start = {.tli = 0,.lsn = InvalidXLogRecPtr},
};

/*
 * Must be the same as in replication/walsender.c
 *
 * This is used to calculate the encryption buffer size.
 */
#define MAX_SEND_SIZE (XLOG_BLCKSZ * 16)

/*
 * Since the backend code needs to use atomics and shared memory while the
 * frotnend code cannot do that we provide two separate implementations of some
 * data structures and the functions which operate one them.
 */

#ifndef FRONTEND

typedef struct EncryptionStateData
{
	/*
	 * To sync with readers. We sync on LSN only and TLI here just to
	 * communicate its value to readers.
	 */
	pg_atomic_uint32 enc_key_tli;
	pg_atomic_uint64 enc_key_lsn;
} EncryptionStateData;

static EncryptionStateData *EncryptionState = NULL;

static char *EncryptionBuf;

static XLogRecPtr
TDEXLogGetEncKeyLsn()
{
	return (XLogRecPtr) pg_atomic_read_u64(&EncryptionState->enc_key_lsn);
}

static TimeLineID
TDEXLogGetEncKeyTli()
{
	return (TimeLineID) pg_atomic_read_u32(&EncryptionState->enc_key_tli);
}

static void
TDEXLogSetEncKeyLocation(WalLocation loc)
{
	/*
	 * Write TLI first and then LSN. The barrier ensures writes won't be
	 * reordered. When reading, the opposite must be done (with a matching
	 * barrier in between), so we always see a valid TLI after observing a
	 * valid LSN.
	 */
	pg_atomic_write_u32(&EncryptionState->enc_key_tli, loc.tli);
	pg_write_barrier();
	pg_atomic_write_u64(&EncryptionState->enc_key_lsn, loc.lsn);
}

static Size TDEXLogEncryptBuffSize(void);

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
	sz = add_size(sz, TDEXLogEncryptBuffSize());
	sz = add_size(sz, PG_IO_ALIGN_SIZE);

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

	EncryptionState = (EncryptionStateData *)
		ShmemInitStruct("TDE XLog Encryption State",
						TDEXLogEncryptStateSize(),
						&foundBuf);

	memset(EncryptionState, 0, sizeof(EncryptionStateData));

	EncryptionBuf = (char *) TYPEALIGN(PG_IO_ALIGN_SIZE, ((char *) EncryptionState) + sizeof(EncryptionStateData));

	Assert((char *) EncryptionState + TDEXLogEncryptStateSize() >= (char *) EncryptionBuf + TDEXLogEncryptBuffSize());

	pg_atomic_init_u64(&EncryptionState->enc_key_lsn, 0);

	elog(DEBUG1, "pg_tde: initialized encryption buffer %lu bytes", TDEXLogEncryptStateSize());
}

#else							/* !FRONTEND */

typedef struct EncryptionStateData
{
	TimeLineID	enc_key_tli;
	XLogRecPtr	enc_key_lsn;
} EncryptionStateData;

static EncryptionStateData EncryptionStateD = {0};

static EncryptionStateData *EncryptionState = &EncryptionStateD;

static char EncryptionBuf[MAX_SEND_SIZE];

static XLogRecPtr
TDEXLogGetEncKeyLsn()
{
	return (XLogRecPtr) EncryptionState->enc_key_lsn;
}

static TimeLineID
TDEXLogGetEncKeyTli()
{
	return (TimeLineID) EncryptionState->enc_key_tli;
}

static void
TDEXLogSetEncKeyLocation(WalLocation loc)
{
	EncryptionState->enc_key_tli = loc.tli;
	EncryptionState->enc_key_lsn = loc.lsn;
}

#endif							/* FRONTEND */

void
TDEXLogSmgrInit()
{
	SetXLogSmgr(&tde_xlog_smgr);
}

void
TDEXLogSmgrInitWrite(bool encrypt_xlog)
{
	WalEncryptionKey *key = pg_tde_read_last_wal_key();
	WALKeyCacheRec *keys;

	/*
	 * Always generate a new key on starting PostgreSQL to protect against
	 * attacks on CTR ciphers based on comparing the WAL generated by two
	 * divergent copies of the same cluster.
	 */
	if (encrypt_xlog)
	{
		pg_tde_create_wal_key(&EncryptionKey, WAL_KEY_TYPE_ENCRYPTED);
	}
	else if (key && key->type == WAL_KEY_TYPE_ENCRYPTED)
	{
		pg_tde_create_wal_key(&EncryptionKey, WAL_KEY_TYPE_UNENCRYPTED);
	}
	else if (key)
	{
		EncryptionKey = *key;
		TDEXLogSetEncKeyLocation(EncryptionKey.wal_start);
	}

	keys = pg_tde_get_wal_cache_keys();

	if (keys == NULL)
	{
		WalLocation start = {.tli = 1,.lsn = 0};

		/* cache is empty, prefetch keys from disk */
		pg_tde_fetch_wal_keys(start);
		pg_tde_wal_cache_extra_palloc();
	}

	if (key)
		pfree(key);
}

void
TDEXLogSmgrInitWriteReuseKey()
{
	WalEncryptionKey *key = pg_tde_read_last_wal_key();

	if (key)
	{
		EncryptionKey = *key;
		TDEXLogSetEncKeyLocation(EncryptionKey.wal_start);
		pfree(key);
	}
}

/*
 * Encrypt XLog page(s) from the buf and write to the segment file.
 */
static ssize_t
TDEXLogWriteEncryptedPagesOldKeys(int fd, const void *buf, size_t count, off_t offset,
								  TimeLineID tli, XLogSegNo segno, int segSize)
{
	char	   *enc_buff = EncryptionBuf;

#ifndef FRONTEND
	Assert(count <= TDEXLogEncryptBuffSize());
#endif

	/* Copy the data as-is, as we might have unencrypted parts */
	memcpy(enc_buff, buf, count);

	/*
	 * This method potentially allocates, but only in very early execution Can
	 * happen during a write, but we have one more cache entry preallocated.
	 */
	TDEXLogCryptBuffer(buf, enc_buff, count, offset, tli, segno, segSize);

	return pg_pwrite(fd, enc_buff, count, offset);
}


/*
 * Encrypt XLog page(s) from the buf and write to the segment file.
 */
static ssize_t
TDEXLogWriteEncryptedPages(int fd, const void *buf, size_t count, off_t offset,
						   TimeLineID tli, XLogSegNo segno)
{
	char		iv_prefix[16];
	WalEncryptionKey *key = &EncryptionKey;
	char	   *enc_buff = EncryptionBuf;

#ifndef FRONTEND
	Assert(count <= TDEXLogEncryptBuffSize());
#endif

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "write encrypted WAL, size: %lu, offset: %ld [%lX], seg: %X/%X, key_start_lsn: %u_%X/%X",
		 count, offset, offset, LSN_FORMAT_ARGS(segno), key->wal_start.tli, LSN_FORMAT_ARGS(key->wal_start.lsn));
#endif

	CalcXLogPageIVPrefix(tli, segno, key->base_iv, iv_prefix);

	pg_tde_stream_crypt(iv_prefix,
						offset,
						(char *) buf,
						count,
						enc_buff,
						key->key,
						&EncryptionCryptCtx);

	return pg_pwrite(fd, enc_buff, count, offset);
}

static ssize_t
tdeheap_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset,
					   TimeLineID tli, XLogSegNo segno, int segSize)
{
	bool		lastKeyUsable;
	bool		afterWriteKey;
#ifdef FRONTEND
	bool		crashRecovery = false;
#else
	bool		crashRecovery = GetRecoveryState() == RECOVERY_STATE_CRASH;
#endif

	WalLocation loc = {.tli = tli};
	WalLocation writeKeyLoc;

	XLogSegNoOffsetToRecPtr(segno, offset, segSize, loc.lsn);

	/*
	 * Set the last (most recent) key's start LSN if not set.
	 *
	 * This func called with WALWriteLock held, so no need in any extra sync.
	 */

	writeKeyLoc.lsn = TDEXLogGetEncKeyLsn();
	pg_read_barrier();
	writeKeyLoc.tli = TDEXLogGetEncKeyTli();

	lastKeyUsable = (writeKeyLoc.lsn != 0);
	afterWriteKey = wal_location_cmp(writeKeyLoc, loc) <= 0;

	if (EncryptionKey.type != WAL_KEY_TYPE_INVALID && !lastKeyUsable && afterWriteKey)
	{
		WALKeyCacheRec *last_key = pg_tde_get_last_wal_key();

		if (!crashRecovery)
		{
			if (last_key == NULL || last_key->start.lsn < loc.lsn)
			{
				pg_tde_wal_last_key_set_location(loc);
				EncryptionKey.wal_start = loc;
				TDEXLogSetEncKeyLocation(EncryptionKey.wal_start);
				lastKeyUsable = true;
			}
		}
	}

	if ((!afterWriteKey || !lastKeyUsable) && EncryptionKey.type != WAL_KEY_TYPE_INVALID)
	{
		return TDEXLogWriteEncryptedPagesOldKeys(fd, buf, count, offset, tli, segno, segSize);
	}
	else if (EncryptionKey.type == WAL_KEY_TYPE_ENCRYPTED)
	{
		return TDEXLogWriteEncryptedPages(fd, buf, count, offset, tli, segno);
	}
	else
	{
		return pg_pwrite(fd, buf, count, offset);
	}
}

/*
 * Read the XLog pages from the segment file and dectypt if need.
 */
static ssize_t
tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset,
					  TimeLineID tli, XLogSegNo segno, int segSize)
{
	ssize_t		readsz;

#ifdef TDE_XLOG_DEBUG
	elog(DEBUG1, "read from a WAL segment, size: %lu offset: %ld [%lX], seg: %u_%X/%X",
		 count, offset, offset, tli, LSN_FORMAT_ARGS(segno));
#endif

	readsz = pg_pread(fd, buf, count, offset);

	if (readsz <= 0)
		return readsz;

	TDEXLogCryptBuffer(buf, buf, count, offset, tli, segno, segSize);

	return readsz;
}

/*
 * [De]Crypt buffer if needed based on provided segment offset, number and TLI
 */
void
TDEXLogCryptBuffer(const void *buf, void *out_buf, size_t count, off_t offset,
				   TimeLineID tli, XLogSegNo segno, int segSize)
{
	WALKeyCacheRec *keys = pg_tde_get_wal_cache_keys();
	XLogRecPtr	write_key_lsn;
	WalLocation data_end = {.tli = tli};
	WalLocation data_start = {.tli = tli};

	if (keys == NULL)
	{
		WalLocation start = {.tli = 1,.lsn = 0};

		/* cache is empty, try to read keys from disk */
		keys = pg_tde_fetch_wal_keys(start);
	}

	/*
	 * The barrier ensures that we always read a vaild TLI after the valid
	 * LSN. See the comment in TDEXLogSetEncKeyLocation()
	 */
	write_key_lsn = TDEXLogGetEncKeyLsn();
	pg_read_barrier();

	if (!XLogRecPtrIsInvalid(write_key_lsn))
	{
		WALKeyCacheRec *last_key = pg_tde_get_last_wal_key();
		WalLocation write_loc = {.tli = TDEXLogGetEncKeyTli(),.lsn = write_key_lsn};

		/* write has generated a new key, need to fetch it */
		if (last_key != NULL && wal_location_cmp(last_key->start, write_loc) < 0)
		{
			pg_tde_fetch_wal_keys(write_loc);

			/* in case cache was empty before */
			keys = pg_tde_get_wal_cache_keys();
		}
	}

	XLogSegNoOffsetToRecPtr(segno, offset, segSize, data_start.lsn);
	XLogSegNoOffsetToRecPtr(segno, offset + count, segSize, data_end.lsn);

	/*
	 * TODO: this is higly ineffective. We should get rid of linked list and
	 * search from the last key as this is what the walsender is useing.
	 */
	for (WALKeyCacheRec *curr_key = keys; curr_key != NULL; curr_key = curr_key->next)
	{
#ifdef TDE_XLOG_DEBUG
		elog(DEBUG1, "WAL key %u_%X/%X - %u_%X/%X, encrypted: %s",
			 curr_key->start.tli, LSN_FORMAT_ARGS(curr_key->start.lsn),
			 curr_key->end.tli, LSN_FORMAT_ARGS(curr_key->end.lsn),
			 curr_key->key.type == WAL_KEY_TYPE_ENCRYPTED ? "yes" : "no");
#endif

		if (wal_location_valid(curr_key->key.wal_start) &&
			curr_key->key.type == WAL_KEY_TYPE_ENCRYPTED)
		{
			/*
			 * Check if the key's range overlaps with the buffer's and decypt
			 * the part that does.
			 */
			if (wal_location_cmp(data_start, curr_key->end) < 0 && wal_location_cmp(data_end, curr_key->start) > 0)
			{
				char		iv_prefix[16];

				/*
				 * We want to calculate where to start / end encrypting. This
				 * depends on two factors:
				 *
				 * 1. Where does the key start / end
				 *
				 * 2. Where does the data start / end
				 *
				 * And this is complicated even more by the fact that keys can
				 * span multiple timelines: if a key starts at TLI 3 LSN 100,
				 * and ends at TLI 5 LSN 200 it means it is used for
				 * everything between two, including the entire TLI 4. For
				 * example, TLI 4 LSN 1 and TLI 4 LSN 400 are both encrypted
				 * with it, even through 1 is less than 100 and 400 is greater
				 * than 200.
				 *
				 * The below min/max calculations make sure that if the key
				 * and data are in the same timeline, we only encrypt/decrypt
				 * in the range of the current key - if the data is longer in
				 * some directions, we use multiple keys. But if the data
				 * starts/ends in a TLI "within" the key, we can safely
				 * decrypt/encrypt from the beginning / until the end, as it
				 * is part of the key.
				 */


				size_t		end_lsn =
					data_end.tli < curr_key->end.tli ? data_end.lsn :
					Min(data_end.lsn, curr_key->end.lsn);
				size_t		start_lsn =
					data_start.tli > curr_key->start.tli ? data_start.lsn :
					Max(data_start.lsn, curr_key->start.lsn);
				off_t		dec_off =
					XLogSegmentOffset(start_lsn, segSize);
				off_t		dec_end =
					XLogSegmentOffset(end_lsn, segSize);
				size_t		dec_sz;
				char	   *dec_buf = (char *) buf + (dec_off - offset);
				char	   *o_buf = (char *) out_buf + (dec_off - offset);

				Assert(dec_off >= offset);

				CalcXLogPageIVPrefix(tli, segno, curr_key->key.base_iv,
									 iv_prefix);

				/*
				 * We have reached the end of the segment
				 */
				if (dec_end == 0)
				{
					dec_end = offset + count;
				}

				Assert(dec_end > dec_off);
				dec_sz = dec_end - dec_off;

#ifdef TDE_XLOG_DEBUG
				elog(DEBUG1, "decrypt WAL, dec_off: %lu [buff_off %lu], sz: %lu | key %u_%X/%X",
					 dec_off, dec_off - offset, dec_sz, curr_key->key.wal_start.tli, LSN_FORMAT_ARGS(curr_key->key.wal_start.lsn));
#endif

				pg_tde_stream_crypt(iv_prefix,
									dec_off,
									dec_buf,
									dec_sz,
									o_buf,
									curr_key->key.key,
									&curr_key->crypt_ctx);
			}
		}
	}
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
