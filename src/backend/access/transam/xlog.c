/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		PostgreSQL transaction log manager
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/access/transam/xlog.c,v 1.60 2001/03/17 20:54:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef USE_LOCALE
#include <locale.h>
#endif

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "storage/sinval.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "storage/s_lock.h"
#include "storage/bufpage.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "utils/builtins.h"
#include "utils/relcache.h"
#include "miscadmin.h"


/*
 * This chunk of hackery attempts to determine which file sync methods
 * are available on the current platform, and to choose an appropriate
 * default method.  We assume that fsync() is always available, and that
 * configure determined whether fdatasync() is.
 */
#define SYNC_METHOD_FSYNC		0
#define SYNC_METHOD_FDATASYNC	1
#define SYNC_METHOD_OPEN		2 /* used for both O_SYNC and O_DSYNC */

#if defined(O_SYNC)
# define OPEN_SYNC_FLAG		O_SYNC
#else
# if defined(O_FSYNC)
#  define OPEN_SYNC_FLAG	O_FSYNC
# endif
#endif

#if defined(OPEN_SYNC_FLAG)
# if defined(O_DSYNC) && (O_DSYNC != OPEN_SYNC_FLAG)
#  define OPEN_DATASYNC_FLAG	O_DSYNC
# endif
#endif

#if defined(OPEN_DATASYNC_FLAG)
# define DEFAULT_SYNC_METHOD_STR	"open_datasync"
# define DEFAULT_SYNC_METHOD		SYNC_METHOD_OPEN
# define DEFAULT_SYNC_FLAGBIT		OPEN_DATASYNC_FLAG
#else
# if defined(HAVE_FDATASYNC)
#  define DEFAULT_SYNC_METHOD_STR	"fdatasync"
#  define DEFAULT_SYNC_METHOD		SYNC_METHOD_FDATASYNC
#  define DEFAULT_SYNC_FLAGBIT		0
# else
#  define DEFAULT_SYNC_METHOD_STR	"fsync"
#  define DEFAULT_SYNC_METHOD		SYNC_METHOD_FSYNC
#  define DEFAULT_SYNC_FLAGBIT		0
# endif
#endif


/* Max time to wait to acquire XLog activity locks */
#define XLOG_LOCK_TIMEOUT			(5*60*1000000) /* 5 minutes */
/* Max time to wait to acquire checkpoint lock */
#define CHECKPOINT_LOCK_TIMEOUT		(20*60*1000000) /* 20 minutes */

/* User-settable parameters */
int			CheckPointSegments = 3;
int			XLOGbuffers = 8;
int			XLOGfiles = 0;	/* how many files to pre-allocate during ckpt */
int			XLOG_DEBUG = 0;
char	   *XLOG_sync_method = NULL;
const char	XLOG_sync_method_default[] = DEFAULT_SYNC_METHOD_STR;
char		XLOG_archive_dir[MAXPGPATH]; /* null string means delete 'em */

/* these are derived from XLOG_sync_method by assign_xlog_sync_method */
static int	sync_method = DEFAULT_SYNC_METHOD;
static int	open_sync_bit = DEFAULT_SYNC_FLAGBIT;

#define MinXLOGbuffers	4

#define XLOG_SYNC_BIT  (enableFsync ? open_sync_bit : 0)


/*
 * ThisStartUpID will be same in all backends --- it identifies current
 * instance of the database system.
 */
StartUpID	ThisStartUpID = 0;

/* Are we doing recovery by reading XLOG? */
bool		InRecovery = false;

/*
 * MyLastRecPtr points to the start of the last XLOG record inserted by the
 * current transaction.  If MyLastRecPtr.xrecoff == 0, then we are not in
 * a transaction or the transaction has not yet made any loggable changes.
 *
 * Note that XLOG records inserted outside transaction control are not
 * reflected into MyLastRecPtr.
 */
XLogRecPtr	MyLastRecPtr = {0, 0};

/*
 * ProcLastRecPtr points to the start of the last XLOG record inserted by the
 * current backend.  It is updated for all inserts, transaction-controlled
 * or not.
 */
static XLogRecPtr ProcLastRecPtr = {0, 0};

/*
 * RedoRecPtr is this backend's local copy of the REDO record pointer
 * (which is almost but not quite the same as a pointer to the most recent
 * CHECKPOINT record).  We update this from the shared-memory copy,
 * XLogCtl->Insert.RedoRecPtr, whenever we can safely do so (ie, when we
 * hold the Insert spinlock).  See XLogInsert for details.
 */
static XLogRecPtr RedoRecPtr;

/* This lock must be held to read/update control file or create new log file */
SPINLOCK	ControlFileLockId;

/*----------
 * Shared-memory data structures for XLOG control
 *
 * LogwrtRqst indicates a byte position that we need to write and/or fsync
 * the log up to (all records before that point must be written or fsynced).
 * LogwrtResult indicates the byte positions we have already written/fsynced.
 * These structs are identical but are declared separately to indicate their
 * slightly different functions.
 *
 * We do a lot of pushups to minimize the amount of access to spinlocked
 * shared memory values.  There are actually three shared-memory copies of
 * LogwrtResult, plus one unshared copy in each backend.  Here's how it works:
 *		XLogCtl->LogwrtResult is protected by info_lck
 *		XLogCtl->Write.LogwrtResult is protected by logwrt_lck
 *		XLogCtl->Insert.LogwrtResult is protected by insert_lck
 * One must hold the associated spinlock to read or write any of these, but
 * of course no spinlock is needed to read/write the unshared LogwrtResult.
 *
 * XLogCtl->LogwrtResult and XLogCtl->Write.LogwrtResult are both "always
 * right", since both are updated by a write or flush operation before
 * it releases logwrt_lck.  The point of keeping XLogCtl->Write.LogwrtResult
 * is that it can be examined/modified by code that already holds logwrt_lck
 * without needing to grab info_lck as well.
 *
 * XLogCtl->Insert.LogwrtResult may lag behind the reality of the other two,
 * but is updated when convenient.  Again, it exists for the convenience of
 * code that is already holding insert_lck but not the other locks.
 *
 * The unshared LogwrtResult may lag behind any or all of these, and again
 * is updated when convenient.
 *
 * The request bookkeeping is simpler: there is a shared XLogCtl->LogwrtRqst
 * (protected by info_lck), but we don't need to cache any copies of it.
 *
 * Note that this all works because the request and result positions can only
 * advance forward, never back up, and so we can easily determine which of two
 * values is "more up to date".
 *----------
 */
typedef struct XLogwrtRqst
{
	XLogRecPtr	Write;			/* last byte + 1 to write out */
	XLogRecPtr	Flush;			/* last byte + 1 to flush */
} XLogwrtRqst;

typedef struct XLogwrtResult
{
	XLogRecPtr	Write;			/* last byte + 1 written out */
	XLogRecPtr	Flush;			/* last byte + 1 flushed */
} XLogwrtResult;

/*
 * Shared state data for XLogInsert.
 */
typedef struct XLogCtlInsert
{
	XLogwrtResult	LogwrtResult;	/* a recent value of LogwrtResult */
	XLogRecPtr		PrevRecord;		/* start of previously-inserted record */
	uint16			curridx;		/* current block index in cache */
	XLogPageHeader	currpage;		/* points to header of block in cache */
	char		   *currpos;		/* current insertion point in cache */
	XLogRecPtr		RedoRecPtr;		/* current redo point for insertions */
} XLogCtlInsert;

/*
 * Shared state data for XLogWrite/XLogFlush.
 */
typedef struct XLogCtlWrite
{
	XLogwrtResult	LogwrtResult;	/* current value of LogwrtResult */
	uint16			curridx;		/* cache index of next block to write */
} XLogCtlWrite;

/*
 * Total shared-memory state for XLOG.
 */
typedef struct XLogCtlData
{
	/* Protected by insert_lck: */
	XLogCtlInsert	Insert;
	/* Protected by info_lck: */
	XLogwrtRqst		LogwrtRqst;
	XLogwrtResult	LogwrtResult;
	/* Protected by logwrt_lck: */
	XLogCtlWrite	Write;
	/*
	 * These values do not change after startup, although the pointed-to
	 * pages and xlblocks values certainly do.  Permission to read/write
	 * the pages and xlblocks values depends on insert_lck and logwrt_lck.
	 */
	char		   *pages;			/* buffers for unwritten XLOG pages */
	XLogRecPtr	   *xlblocks;		/* 1st byte ptr-s + BLCKSZ */
	uint32			XLogCacheByte;	/* # bytes in xlog buffers */
	uint32			XLogCacheBlck;	/* highest allocated xlog buffer index */
	StartUpID		ThisStartUpID;

	/* This value is not protected by *any* spinlock... */
	XLogRecPtr		RedoRecPtr;		/* see SetRedoRecPtr/GetRedoRecPtr */

	slock_t			insert_lck;		/* XLogInsert lock */
	slock_t			info_lck;		/* locks shared LogwrtRqst/LogwrtResult */
	slock_t			logwrt_lck;		/* XLogWrite/XLogFlush lock */
	slock_t			chkp_lck;		/* checkpoint lock */
} XLogCtlData;

static XLogCtlData *XLogCtl = NULL;

/*
 * We maintain an image of pg_control in shared memory.
 */
static ControlFileData *ControlFile = NULL;

/*
 * Macros for managing XLogInsert state.  In most cases, the calling routine
 * has local copies of XLogCtl->Insert and/or XLogCtl->Insert->curridx,
 * so these are passed as parameters instead of being fetched via XLogCtl.
 */

/* Free space remaining in the current xlog page buffer */
#define INSERT_FREESPACE(Insert)  \
	(BLCKSZ - ((Insert)->currpos - (char *) (Insert)->currpage))

/* Construct XLogRecPtr value for current insertion point */
#define INSERT_RECPTR(recptr,Insert,curridx)  \
	( \
	  (recptr).xlogid = XLogCtl->xlblocks[curridx].xlogid, \
	  (recptr).xrecoff = \
	  	XLogCtl->xlblocks[curridx].xrecoff - INSERT_FREESPACE(Insert) \
	)


/* Increment an xlogid/segment pair */
#define NextLogSeg(logId, logSeg)	\
	do { \
		if ((logSeg) >= XLogSegsPerFile-1) \
		{ \
			(logId)++; \
			(logSeg) = 0; \
		} \
		else \
			(logSeg)++; \
	} while (0)

/* Decrement an xlogid/segment pair (assume it's not 0,0) */
#define PrevLogSeg(logId, logSeg)	\
	do { \
		if (logSeg) \
			(logSeg)--; \
		else \
		{ \
			(logId)--; \
			(logSeg) = XLogSegsPerFile-1; \
		} \
	} while (0)

/*
 * Compute ID and segment from an XLogRecPtr.
 *
 * For XLByteToSeg, do the computation at face value.  For XLByteToPrevSeg,
 * a boundary byte is taken to be in the previous segment.  This is suitable
 * for deciding which segment to write given a pointer to a record end,
 * for example.
 */
#define XLByteToSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = (xlrp).xrecoff / XLogSegSize \
	)
#define XLByteToPrevSeg(xlrp, logId, logSeg)	\
	( logId = (xlrp).xlogid, \
	  logSeg = ((xlrp).xrecoff - 1) / XLogSegSize \
	)

/*
 * Is an XLogRecPtr within a particular XLOG segment?
 *
 * For XLByteInSeg, do the computation at face value.  For XLByteInPrevSeg,
 * a boundary byte is taken to be in the previous segment.
 */
#define XLByteInSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 (xlrp).xrecoff / XLogSegSize == (logSeg))

#define XLByteInPrevSeg(xlrp, logId, logSeg)	\
	((xlrp).xlogid == (logId) && \
	 ((xlrp).xrecoff - 1) / XLogSegSize == (logSeg))


#define XLogFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%c%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

#define PrevBufIdx(idx)		\
		(((idx) == 0) ? XLogCtl->XLogCacheBlck : ((idx) - 1))

#define NextBufIdx(idx)		\
		(((idx) == XLogCtl->XLogCacheBlck) ? 0 : ((idx) + 1))

#define XRecOffIsValid(xrecoff) \
		((xrecoff) % BLCKSZ >= SizeOfXLogPHD && \
		(BLCKSZ - (xrecoff) % BLCKSZ) >= SizeOfXLogRecord)

/*
 * _INTL_MAXLOGRECSZ: max space needed for a record including header and
 * any backup-block data.
 */
#define _INTL_MAXLOGRECSZ	(SizeOfXLogRecord + MAXLOGRECSZ + \
							 XLR_MAX_BKP_BLOCKS * (sizeof(BkpBlock) + BLCKSZ))


/* File path names */
static char		XLogDir[MAXPGPATH];
static char		ControlFilePath[MAXPGPATH];

/*
 * Private, possibly out-of-date copy of shared LogwrtResult.
 * See discussion above.
 */
static XLogwrtResult LogwrtResult = {{0, 0}, {0, 0}};

/*
 * openLogFile is -1 or a kernel FD for an open log file segment.
 * When it's open, openLogOff is the current seek offset in the file.
 * openLogId/openLogSeg identify the segment.  These variables are only
 * used to write the XLOG, and so will normally refer to the active segment.
 */
static int	openLogFile = -1;
static uint32 openLogId = 0;
static uint32 openLogSeg = 0;
static uint32 openLogOff = 0;

/*
 * These variables are used similarly to the ones above, but for reading
 * the XLOG.  Note, however, that readOff generally represents the offset
 * of the page just read, not the seek position of the FD itself, which
 * will be just past that page.
 */
static int	readFile = -1;
static uint32 readId = 0;
static uint32 readSeg = 0;
static uint32 readOff = 0;
/* Buffer for currently read page (BLCKSZ bytes) */
static char *readBuf = NULL;
/* State information for XLOG reading */
static XLogRecPtr ReadRecPtr;
static XLogRecPtr EndRecPtr;
static XLogRecord *nextRecord = NULL;
static StartUpID lastReadSUI;

static bool InRedo = false;


static bool AdvanceXLInsertBuffer(void);
static void XLogWrite(XLogwrtRqst WriteRqst);
static int	XLogFileInit(uint32 log, uint32 seg,
						 bool *use_existent, bool use_lock);
static int	XLogFileOpen(uint32 log, uint32 seg, bool econt);
static void PreallocXlogFiles(XLogRecPtr endptr);
static void MoveOfflineLogs(uint32 log, uint32 seg);
static XLogRecord *ReadRecord(XLogRecPtr *RecPtr, int emode, char *buffer);
static bool ValidXLOGHeader(XLogPageHeader hdr, int emode, bool checkSUI);
static XLogRecord *ReadCheckpointRecord(XLogRecPtr RecPtr,
										const char *whichChkpt,
										char *buffer);
static void WriteControlFile(void);
static void ReadControlFile(void);
static char *str_time(time_t tnow);
static void xlog_outrec(char *buf, XLogRecord *record);
static void issue_xlog_fsync(void);


/*
 * Insert an XLOG record having the specified RMID and info bytes,
 * with the body of the record being the data chunk(s) described by
 * the rdata list (see xlog.h for notes about rdata).
 *
 * Returns XLOG pointer to end of record (beginning of next record).
 * This can be used as LSN for data pages affected by the logged action.
 * (LSN is the XLOG point up to which the XLOG must be flushed to disk
 * before the data page can be written out.  This implements the basic
 * WAL rule "write the log before the data".)
 *
 * NB: this routine feels free to scribble on the XLogRecData structs,
 * though not on the data they reference.  This is OK since the XLogRecData
 * structs are always just temporaries in the calling code.
 */
XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info, XLogRecData *rdata)
{
	XLogCtlInsert  *Insert = &XLogCtl->Insert;
	XLogRecord	   *record;
	XLogContRecord *contrecord;
	XLogRecPtr		RecPtr;
	XLogRecPtr		WriteRqst;
	uint32			freespace;
	uint16			curridx;
	XLogRecData	   *rdt;
	Buffer			dtbuf[XLR_MAX_BKP_BLOCKS];
	bool			dtbuf_bkp[XLR_MAX_BKP_BLOCKS];
	BkpBlock		dtbuf_xlg[XLR_MAX_BKP_BLOCKS];
	XLogRecPtr		dtbuf_lsn[XLR_MAX_BKP_BLOCKS];
	XLogRecData		dtbuf_rdt[2 * XLR_MAX_BKP_BLOCKS];
	crc64			rdata_crc;
	uint32			len,
					write_len;
	unsigned		i;
	bool			do_logwrt;
	bool			updrqst;
	bool			no_tran = (rmid == RM_XLOG_ID) ? true : false;

	if (info & XLR_INFO_MASK)
	{
		if ((info & XLR_INFO_MASK) != XLOG_NO_TRAN)
			elog(STOP, "XLogInsert: invalid info mask %02X", 
				 (info & XLR_INFO_MASK));
		no_tran = true;
		info &= ~XLR_INFO_MASK;
	}

	/*
	 * In bootstrap mode, we don't actually log anything but XLOG resources;
	 * return a phony record pointer.
	 */
	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		RecPtr.xlogid = 0;
		RecPtr.xrecoff = SizeOfXLogPHD;	/* start of 1st checkpoint record */
		return (RecPtr);
	}

	/*
	 * Here we scan the rdata list, determine which buffers must be backed
	 * up, and compute the CRC values for the data.  Note that the record
	 * header isn't added into the CRC yet since we don't know the final
	 * length or info bits quite yet.
	 *
	 * We may have to loop back to here if a race condition is detected below.
	 * We could prevent the race by doing all this work while holding the
	 * insert spinlock, but it seems better to avoid doing CRC calculations
	 * while holding the lock.  This means we have to be careful about
	 * modifying the rdata list until we know we aren't going to loop back
	 * again.  The only change we allow ourselves to make earlier is to set
	 * rdt->data = NULL in list items we have decided we will have to back
	 * up the whole buffer for.  This is OK because we will certainly decide
	 * the same thing again for those items if we do it over; doing it here
	 * saves an extra pass over the list later.
	 */
begin:;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		dtbuf[i] = InvalidBuffer;
		dtbuf_bkp[i] = false;
	}

	INIT_CRC64(rdata_crc);
	len = 0;
	for (rdt = rdata; ; )
	{
		if (rdt->buffer == InvalidBuffer)
		{
			/* Simple data, just include it */
			len += rdt->len;
			COMP_CRC64(rdata_crc, rdt->data, rdt->len);
		}
		else
		{
			/* Find info for buffer */
			for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
			{
				if (rdt->buffer == dtbuf[i])
				{
					/* Buffer already referenced by earlier list item */
					if (dtbuf_bkp[i])
						rdt->data = NULL;
					else if (rdt->data)
					{
						len += rdt->len;
						COMP_CRC64(rdata_crc, rdt->data, rdt->len);
					}
					break;
				}
				if (dtbuf[i] == InvalidBuffer)
				{
					/* OK, put it in this slot */
					dtbuf[i] = rdt->buffer;
					/*
					 * XXX We assume page LSN is first data on page
					 */
					dtbuf_lsn[i] = *((XLogRecPtr*)BufferGetBlock(rdt->buffer));
					if (XLByteLE(dtbuf_lsn[i], RedoRecPtr))
					{
						crc64	dtcrc;

						dtbuf_bkp[i] = true;
						rdt->data = NULL;
						INIT_CRC64(dtcrc);
						COMP_CRC64(dtcrc,
								   BufferGetBlock(dtbuf[i]),
								   BLCKSZ);
						dtbuf_xlg[i].node = BufferGetFileNode(dtbuf[i]);
						dtbuf_xlg[i].block = BufferGetBlockNumber(dtbuf[i]);
						COMP_CRC64(dtcrc,
								   (char*) &(dtbuf_xlg[i]) + sizeof(crc64),
								   sizeof(BkpBlock) - sizeof(crc64));
						FIN_CRC64(dtcrc);
						dtbuf_xlg[i].crc = dtcrc;
					}
					else if (rdt->data)
					{
						len += rdt->len;
						COMP_CRC64(rdata_crc, rdt->data, rdt->len);
					}
					break;
				}
			}
			if (i >= XLR_MAX_BKP_BLOCKS)
				elog(STOP, "XLogInsert: can backup %d blocks at most",
					 XLR_MAX_BKP_BLOCKS);
		}
		/* Break out of loop when rdt points to last list item */
		if (rdt->next == NULL)
			break;
		rdt = rdt->next;
	}

	/*
	 * NOTE: the test for len == 0 here is somewhat fishy, since in theory
	 * all of the rmgr data might have been suppressed in favor of backup
	 * blocks.  Currently, all callers of XLogInsert provide at least some
	 * not-in-a-buffer data and so len == 0 should never happen, but that
	 * may not be true forever.  If you need to remove the len == 0 check,
	 * also remove the check for xl_len == 0 in ReadRecord, below.
	 */
	if (len == 0 || len > MAXLOGRECSZ)
		elog(STOP, "XLogInsert: invalid record len %u", len);

	START_CRIT_SECTION();

	/* wait to obtain xlog insert lock */
	do_logwrt = true;

	for (i = 0;;)
	{
		/* try to update LogwrtResult while waiting for insert lock */
		if (!TAS(&(XLogCtl->info_lck)))
		{
			XLogwrtRqst	LogwrtRqst;

			LogwrtRqst = XLogCtl->LogwrtRqst;
			LogwrtResult = XLogCtl->LogwrtResult;
			S_UNLOCK(&(XLogCtl->info_lck));

			/*
			 * If cache is half filled then try to acquire logwrt lock
			 * and do LOGWRT work, but only once per XLogInsert call.
			 * Ignore any fractional blocks in performing this check.
			 */
			LogwrtRqst.Write.xrecoff -= LogwrtRqst.Write.xrecoff % BLCKSZ;
			if (do_logwrt &&
				(LogwrtRqst.Write.xlogid != LogwrtResult.Write.xlogid ||
				 (LogwrtRqst.Write.xrecoff >= LogwrtResult.Write.xrecoff +
				  XLogCtl->XLogCacheByte / 2)))
			{
				if (!TAS(&(XLogCtl->logwrt_lck)))
				{
					LogwrtResult = XLogCtl->Write.LogwrtResult;
					if (XLByteLT(LogwrtResult.Write, LogwrtRqst.Write))
					{
						XLogWrite(LogwrtRqst);
						do_logwrt = false;
					}
					S_UNLOCK(&(XLogCtl->logwrt_lck));
				}
			}
		}
		if (!TAS(&(XLogCtl->insert_lck)))
			break;
		S_LOCK_SLEEP(&(XLogCtl->insert_lck), i++, XLOG_LOCK_TIMEOUT);
	}

	/*
	 * Check to see if my RedoRecPtr is out of date.  If so, may have to
	 * go back and recompute everything.  This can only happen just after a
	 * checkpoint, so it's better to be slow in this case and fast otherwise.
	 */
	if (!XLByteEQ(RedoRecPtr, Insert->RedoRecPtr))
	{
		Assert(XLByteLT(RedoRecPtr, Insert->RedoRecPtr));
		RedoRecPtr = Insert->RedoRecPtr;

		for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
		{
			if (dtbuf[i] == InvalidBuffer)
				continue;
			if (dtbuf_bkp[i] == false &&
				XLByteLE(dtbuf_lsn[i], RedoRecPtr))
			{
				/*
				 * Oops, this buffer now needs to be backed up, but we didn't
				 * think so above.  Start over.
				 */
				S_UNLOCK(&(XLogCtl->insert_lck));
				END_CRIT_SECTION();
				goto begin;
			}
		}
	}

	/*
	 * Make additional rdata list entries for the backup blocks, so that
	 * we don't need to special-case them in the write loop.  Note that we
	 * have now irrevocably changed the input rdata list.  At the exit of
	 * this loop, write_len includes the backup block data.
	 *
	 * Also set the appropriate info bits to show which buffers were backed
	 * up.  The i'th XLR_SET_BKP_BLOCK bit corresponds to the i'th distinct
	 * buffer value (ignoring InvalidBuffer) appearing in the rdata list.
	 */
	write_len = len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (dtbuf[i] == InvalidBuffer || !(dtbuf_bkp[i]))
			continue;

		info |= XLR_SET_BKP_BLOCK(i);

		rdt->next = &(dtbuf_rdt[2 * i]);

		dtbuf_rdt[2 * i].data = (char*) &(dtbuf_xlg[i]);
		dtbuf_rdt[2 * i].len = sizeof(BkpBlock);
		write_len += sizeof(BkpBlock);

		rdt = dtbuf_rdt[2 * i].next = &(dtbuf_rdt[2 * i + 1]);

		dtbuf_rdt[2 * i + 1].data = (char*) BufferGetBlock(dtbuf[i]);
		dtbuf_rdt[2 * i + 1].len = BLCKSZ;
		write_len += BLCKSZ;
		dtbuf_rdt[2 * i + 1].next = NULL;
	}

	/* Insert record header */

	updrqst = false;
	freespace = INSERT_FREESPACE(Insert);
	if (freespace < SizeOfXLogRecord)
	{
		updrqst = AdvanceXLInsertBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD;
	}

	curridx = Insert->curridx;
	record = (XLogRecord *) Insert->currpos;

	record->xl_prev = Insert->PrevRecord;
	if (no_tran)
	{
		record->xl_xact_prev.xlogid = 0;
		record->xl_xact_prev.xrecoff = 0;
	}
	else
		record->xl_xact_prev = MyLastRecPtr;

	record->xl_xid = GetCurrentTransactionId();
	record->xl_len = len;		/* doesn't include backup blocks */
	record->xl_info = info;
	record->xl_rmid = rmid;

	/* Now we can finish computing the main CRC */
	COMP_CRC64(rdata_crc, (char*) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(rdata_crc);
	record->xl_crc = rdata_crc;

	/* Compute record's XLOG location */
	INSERT_RECPTR(RecPtr, Insert, curridx);

	/* If first XLOG record of transaction, save it in PROC array */
	if (MyLastRecPtr.xrecoff == 0 && !no_tran)
	{
		SpinAcquire(SInvalLock);
		MyProc->logRec = RecPtr;
		SpinRelease(SInvalLock);
	}

	if (XLOG_DEBUG)
	{
		char	buf[8192];

		sprintf(buf, "INSERT @ %u/%u: ", RecPtr.xlogid, RecPtr.xrecoff);
		xlog_outrec(buf, record);
		if (rdata->data != NULL)
		{
			strcat(buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(buf, record->xl_info, rdata->data);
		}
		fprintf(stderr, "%s\n", buf);
	}

	/* Record begin of record in appropriate places */
	if (!no_tran)
		MyLastRecPtr = RecPtr;
	ProcLastRecPtr = RecPtr;
	Insert->PrevRecord = RecPtr;

	Insert->currpos += SizeOfXLogRecord;
	freespace -= SizeOfXLogRecord;

	/*
	 * Append the data, including backup blocks if any
	 */
	while (write_len)
	{
		while (rdata->data == NULL)
			rdata = rdata->next;

		if (freespace > 0)
		{
			if (rdata->len > freespace)
			{
				memcpy(Insert->currpos, rdata->data, freespace);
				rdata->data += freespace;
				rdata->len -= freespace;
				write_len -= freespace;
			}
			else
			{
				memcpy(Insert->currpos, rdata->data, rdata->len);
				freespace -= rdata->len;
				write_len -= rdata->len;
				Insert->currpos += rdata->len;
				rdata = rdata->next;
				continue;
			}
		}

		/* Use next buffer */
		updrqst = AdvanceXLInsertBuffer();
		curridx = Insert->curridx;
		/* Insert cont-record header */
		Insert->currpage->xlp_info |= XLP_FIRST_IS_CONTRECORD;
		contrecord = (XLogContRecord *) Insert->currpos;
		contrecord->xl_rem_len = write_len;
		Insert->currpos += SizeOfXLogContRecord;
		freespace = BLCKSZ - SizeOfXLogPHD - SizeOfXLogContRecord;
	}

	/* Ensure next record will be properly aligned */
	Insert->currpos = (char *) Insert->currpage +
			MAXALIGN(Insert->currpos - (char *) Insert->currpage);
	freespace = INSERT_FREESPACE(Insert);

	/*
	 * The recptr I return is the beginning of the *next* record.
	 * This will be stored as LSN for changed data pages...
	 */
	INSERT_RECPTR(RecPtr, Insert, curridx);

	/* Need to update shared LogwrtRqst if some block was filled up */
	if (freespace < SizeOfXLogRecord)
		updrqst = true;	/* curridx is filled and available for writing out */
	else
		curridx = PrevBufIdx(curridx);
	WriteRqst = XLogCtl->xlblocks[curridx];

	S_UNLOCK(&(XLogCtl->insert_lck));

	if (updrqst)
	{
		S_LOCK(&(XLogCtl->info_lck));
		/* advance global request to include new block(s) */
		if (XLByteLT(XLogCtl->LogwrtRqst.Write, WriteRqst))
			XLogCtl->LogwrtRqst.Write = WriteRqst;
		/* update local result copy while I have the chance */
		LogwrtResult = XLogCtl->LogwrtResult;
		S_UNLOCK(&(XLogCtl->info_lck));
	}

	END_CRIT_SECTION();
	return (RecPtr);
}

/*
 * Advance the Insert state to the next buffer page, writing out the next
 * buffer if it still contains unwritten data.
 *
 * The global LogwrtRqst.Write pointer needs to be advanced to include the
 * just-filled page.  If we can do this for free (without an extra spinlock),
 * we do so here.  Otherwise the caller must do it.  We return TRUE if the
 * request update still needs to be done, FALSE if we did it internally.
 *
 * Must be called with insert_lck held.
 */
static bool
AdvanceXLInsertBuffer(void)
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogCtlWrite *Write = &XLogCtl->Write;
	uint16		nextidx = NextBufIdx(Insert->curridx);
	bool		update_needed = true;
	XLogRecPtr	OldPageRqstPtr;
	XLogwrtRqst WriteRqst;

	/* Use Insert->LogwrtResult copy if it's more fresh */
	if (XLByteLT(LogwrtResult.Write, Insert->LogwrtResult.Write))
		LogwrtResult = Insert->LogwrtResult;

	/*
	 * Get ending-offset of the buffer page we need to replace (this may be
	 * zero if the buffer hasn't been used yet).  Fall through if it's already
	 * written out.
	 */
	OldPageRqstPtr = XLogCtl->xlblocks[nextidx];
	if (!XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
	{
		/* nope, got work to do... */
		unsigned	spins = 0;
		XLogRecPtr	FinishedPageRqstPtr;

		FinishedPageRqstPtr = XLogCtl->xlblocks[Insert->curridx];

		for (;;)
		{
			/* While waiting, try to get info_lck and update LogwrtResult */
			if (!TAS(&(XLogCtl->info_lck)))
			{
				if (XLByteLT(XLogCtl->LogwrtRqst.Write, FinishedPageRqstPtr))
					XLogCtl->LogwrtRqst.Write = FinishedPageRqstPtr;
				update_needed = false; /* Did the shared-request update */
				LogwrtResult = XLogCtl->LogwrtResult;
				S_UNLOCK(&(XLogCtl->info_lck));

				if (XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
				{
					/* OK, someone wrote it already */
					Insert->LogwrtResult = LogwrtResult;
					break;
				}
			}

			/*
			 * LogwrtResult lock is busy or we know the page is still dirty.
			 * Try to acquire logwrt lock and write full blocks.
			 */
			if (!TAS(&(XLogCtl->logwrt_lck)))
			{
				LogwrtResult = Write->LogwrtResult;
				if (XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
				{
					S_UNLOCK(&(XLogCtl->logwrt_lck));
					/* OK, someone wrote it already */
					Insert->LogwrtResult = LogwrtResult;
					break;
				}
				/*
				 * Have to write buffers while holding insert lock.
				 * This is not good, so only write as much as we absolutely
				 * must.
				 */
				WriteRqst.Write = OldPageRqstPtr;
				WriteRqst.Flush.xlogid = 0;
				WriteRqst.Flush.xrecoff = 0;
				XLogWrite(WriteRqst);
				S_UNLOCK(&(XLogCtl->logwrt_lck));
				Insert->LogwrtResult = LogwrtResult;
				break;
			}
			S_LOCK_SLEEP(&(XLogCtl->logwrt_lck), spins++, XLOG_LOCK_TIMEOUT);
		}
	}

	/*
	 * Now the next buffer slot is free and we can set it up to be the
	 * next output page.
	 */
	if (XLogCtl->xlblocks[Insert->curridx].xrecoff >= XLogFileSize)
	{
		/* crossing a logid boundary */
		XLogCtl->xlblocks[nextidx].xlogid =
			XLogCtl->xlblocks[Insert->curridx].xlogid + 1;
		XLogCtl->xlblocks[nextidx].xrecoff = BLCKSZ;
	}
	else
	{
		XLogCtl->xlblocks[nextidx].xlogid =
			XLogCtl->xlblocks[Insert->curridx].xlogid;
		XLogCtl->xlblocks[nextidx].xrecoff =
			XLogCtl->xlblocks[Insert->curridx].xrecoff + BLCKSZ;
	}
	Insert->curridx = nextidx;
	Insert->currpage = (XLogPageHeader) (XLogCtl->pages + nextidx * BLCKSZ);
	Insert->currpos = ((char*) Insert->currpage) + SizeOfXLogPHD;
	/*
	 * Be sure to re-zero the buffer so that bytes beyond what we've written
	 * will look like zeroes and not valid XLOG records...
	 */
	MemSet((char*) Insert->currpage, 0, BLCKSZ);
	Insert->currpage->xlp_magic = XLOG_PAGE_MAGIC;
	/* Insert->currpage->xlp_info = 0; */	/* done by memset */
	Insert->currpage->xlp_sui = ThisStartUpID;

	return update_needed;
}

/*
 * Write and/or fsync the log at least as far as WriteRqst indicates.
 *
 * Must be called with logwrt_lck held.
 */
static void
XLogWrite(XLogwrtRqst WriteRqst)
{
	XLogCtlWrite *Write = &XLogCtl->Write;
	char	   *from;
	bool		ispartialpage;
	bool		use_existent;

	/* Update local LogwrtResult (caller probably did this already, but...) */
	LogwrtResult = Write->LogwrtResult;

	while (XLByteLT(LogwrtResult.Write, WriteRqst.Write))
	{
		/*
		 * Make sure we're not ahead of the insert process.  This could
		 * happen if we're passed a bogus WriteRqst.Write that is past the
		 * end of the last page that's been initialized by
		 * AdvanceXLInsertBuffer.
		 */
		if (!XLByteLT(LogwrtResult.Write, XLogCtl->xlblocks[Write->curridx]))
			elog(STOP, "XLogWrite: write request is past end of log");

		/* Advance LogwrtResult.Write to end of current buffer page */
		LogwrtResult.Write = XLogCtl->xlblocks[Write->curridx];
		ispartialpage = XLByteLT(WriteRqst.Write, LogwrtResult.Write);

		if (!XLByteInPrevSeg(LogwrtResult.Write, openLogId, openLogSeg))
		{
			/*
			 * Switch to new logfile segment.
			 */
			if (openLogFile >= 0)
			{
				if (close(openLogFile) != 0)
					elog(STOP, "close(logfile %u seg %u) failed: %m",
						 openLogId, openLogSeg);
				openLogFile = -1;
			}
			XLByteToPrevSeg(LogwrtResult.Write, openLogId, openLogSeg);

			/* create/use new log file */
			use_existent = true;
			openLogFile = XLogFileInit(openLogId, openLogSeg,
									   &use_existent, true);
			openLogOff = 0;

			if (!use_existent)	/* there was no precreated file */
				elog(LOG, "XLogWrite: new log file created - "
					 "consider increasing WAL_FILES");

			/* update pg_control, unless someone else already did */
			SpinAcquire(ControlFileLockId);
			if (ControlFile->logId != openLogId ||
				ControlFile->logSeg != openLogSeg + 1)
			{
				ControlFile->logId = openLogId;
				ControlFile->logSeg = openLogSeg + 1;
				ControlFile->time = time(NULL);
				UpdateControlFile();
				/*
				 * Signal postmaster to start a checkpoint if it's been too
				 * long since the last one.  (We look at local copy of
				 * RedoRecPtr which might be a little out of date, but should
				 * be close enough for this purpose.)
				 */
				if (IsUnderPostmaster &&
					(openLogId != RedoRecPtr.xlogid ||
					 openLogSeg >= (RedoRecPtr.xrecoff / XLogSegSize) +
					 (uint32) CheckPointSegments))
				{
					if (XLOG_DEBUG)
						fprintf(stderr, "XLogWrite: time for a checkpoint, signaling postmaster\n");
					kill(getppid(), SIGUSR1);
				}
			}
			SpinRelease(ControlFileLockId);
		}

		if (openLogFile < 0)
		{
			XLByteToPrevSeg(LogwrtResult.Write, openLogId, openLogSeg);
			openLogFile = XLogFileOpen(openLogId, openLogSeg, false);
			openLogOff = 0;
		}

		/* Need to seek in the file? */
		if (openLogOff != (LogwrtResult.Write.xrecoff - BLCKSZ) % XLogSegSize)
		{
			openLogOff = (LogwrtResult.Write.xrecoff - BLCKSZ) % XLogSegSize;
			if (lseek(openLogFile, (off_t) openLogOff, SEEK_SET) < 0)
				elog(STOP, "lseek(logfile %u seg %u off %u) failed: %m",
					 openLogId, openLogSeg, openLogOff);
		}

		/* OK to write the page */
		from = XLogCtl->pages + Write->curridx * BLCKSZ;
		if (write(openLogFile, from, BLCKSZ) != BLCKSZ)
			elog(STOP, "write(logfile %u seg %u off %u) failed: %m",
				 openLogId, openLogSeg, openLogOff);
		openLogOff += BLCKSZ;

		/*
		 * If we just wrote the whole last page of a logfile segment,
		 * fsync the segment immediately.  This avoids having to go back
		 * and re-open prior segments when an fsync request comes along later.
		 * Doing it here ensures that one and only one backend will perform
		 * this fsync.
		 */
		if (openLogOff >= XLogSegSize && !ispartialpage)
		{
			issue_xlog_fsync();
			LogwrtResult.Flush = LogwrtResult.Write; /* end of current page */
		}

		if (ispartialpage)
		{
			/* Only asked to write a partial page */
			LogwrtResult.Write = WriteRqst.Write;
			break;
		}
		Write->curridx = NextBufIdx(Write->curridx);
	}

	/*
	 * If asked to flush, do so
	 */
	if (XLByteLT(LogwrtResult.Flush, WriteRqst.Flush) &&
		XLByteLT(LogwrtResult.Flush, LogwrtResult.Write))
	{
		/*
		 * Could get here without iterating above loop, in which case
		 * we might have no open file or the wrong one.  However, we do
		 * not need to fsync more than one file.
		 */
		if (sync_method != SYNC_METHOD_OPEN)
		{
			if (openLogFile >= 0 &&
				!XLByteInPrevSeg(LogwrtResult.Write, openLogId, openLogSeg))
			{
				if (close(openLogFile) != 0)
					elog(STOP, "close(logfile %u seg %u) failed: %m",
						 openLogId, openLogSeg);
				openLogFile = -1;
			}
			if (openLogFile < 0)
			{
				XLByteToPrevSeg(LogwrtResult.Write, openLogId, openLogSeg);
				openLogFile = XLogFileOpen(openLogId, openLogSeg, false);
				openLogOff = 0;
			}
			issue_xlog_fsync();
		}
		LogwrtResult.Flush = LogwrtResult.Write;
	}

	/*
	 * Update shared-memory status
	 *
	 * We make sure that the shared 'request' values do not fall behind
	 * the 'result' values.  This is not absolutely essential, but it saves
	 * some code in a couple of places.
	 */
	S_LOCK(&(XLogCtl->info_lck));
	XLogCtl->LogwrtResult = LogwrtResult;
	if (XLByteLT(XLogCtl->LogwrtRqst.Write, LogwrtResult.Write))
		XLogCtl->LogwrtRqst.Write = LogwrtResult.Write;
	if (XLByteLT(XLogCtl->LogwrtRqst.Flush, LogwrtResult.Flush))
		XLogCtl->LogwrtRqst.Flush = LogwrtResult.Flush;
	S_UNLOCK(&(XLogCtl->info_lck));

	Write->LogwrtResult = LogwrtResult;
}

/*
 * Ensure that all XLOG data through the given position is flushed to disk.
 *
 * NOTE: this differs from XLogWrite mainly in that the logwrt_lck is not
 * already held, and we try to avoid acquiring it if possible.
 */
void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqstPtr;
	XLogwrtRqst WriteRqst;
	unsigned	spins = 0;

	if (XLOG_DEBUG)
	{
		fprintf(stderr, "XLogFlush%s%s: rqst %u/%u; wrt %u/%u; flsh %u/%u\n",
				(IsBootstrapProcessingMode()) ? "(bootstrap)" : "",
				(InRedo) ? "(redo)" : "",
				record.xlogid, record.xrecoff,
				LogwrtResult.Write.xlogid, LogwrtResult.Write.xrecoff,
				LogwrtResult.Flush.xlogid, LogwrtResult.Flush.xrecoff);
		fflush(stderr);
	}

	/* Disabled during REDO */
	if (InRedo)
		return;

	/* Quick exit if already known flushed */
	if (XLByteLE(record, LogwrtResult.Flush))
		return;

	START_CRIT_SECTION();

	/*
	 * Since fsync is usually a horribly expensive operation, we try to
	 * piggyback as much data as we can on each fsync: if we see any more
	 * data entered into the xlog buffer, we'll write and fsync that too,
	 * so that the final value of LogwrtResult.Flush is as large as possible.
	 * This gives us some chance of avoiding another fsync immediately after.
	 */

	/* initialize to given target; may increase below */
	WriteRqstPtr = record;

	for (;;)
	{
		/* try to read LogwrtResult and update local state */
		if (!TAS(&(XLogCtl->info_lck)))
		{
			if (XLByteLT(WriteRqstPtr, XLogCtl->LogwrtRqst.Write))
				WriteRqstPtr = XLogCtl->LogwrtRqst.Write;
			LogwrtResult = XLogCtl->LogwrtResult;
			S_UNLOCK(&(XLogCtl->info_lck));
			if (XLByteLE(record, LogwrtResult.Flush))
			{
				/* Done already */
				break;
			}
		}
		/* if something was added to log cache then try to flush this too */
		if (!TAS(&(XLogCtl->insert_lck)))
		{
			XLogCtlInsert *Insert = &XLogCtl->Insert;
			uint32		freespace = INSERT_FREESPACE(Insert);

			if (freespace < SizeOfXLogRecord)	/* buffer is full */
			{
				WriteRqstPtr = XLogCtl->xlblocks[Insert->curridx];
			}
			else
			{
				WriteRqstPtr = XLogCtl->xlblocks[Insert->curridx];
				WriteRqstPtr.xrecoff -= freespace;
			}
			S_UNLOCK(&(XLogCtl->insert_lck));
		}
		/* now try to get the logwrt lock */
		if (!TAS(&(XLogCtl->logwrt_lck)))
		{
			LogwrtResult = XLogCtl->Write.LogwrtResult;
			if (XLByteLE(record, LogwrtResult.Flush))
			{
				/* Done already */
				S_UNLOCK(&(XLogCtl->logwrt_lck));
				break;
			}
			WriteRqst.Write = WriteRqstPtr;
			WriteRqst.Flush = record;
			XLogWrite(WriteRqst);
			S_UNLOCK(&(XLogCtl->logwrt_lck));
			if (XLByteLT(LogwrtResult.Flush, record))
				elog(STOP, "XLogFlush: request is not satisfied");
			break;
		}
		S_LOCK_SLEEP(&(XLogCtl->logwrt_lck), spins++, XLOG_LOCK_TIMEOUT);
	}

	END_CRIT_SECTION();
}

/*
 * Create a new XLOG file segment, or open a pre-existing one.
 *
 * log, seg: identify segment to be created/opened.
 *
 * *use_existent: if TRUE, OK to use a pre-existing file (else, any
 * pre-existing file will be deleted).  On return, TRUE if a pre-existing
 * file was used.
 *
 * use_lock: if TRUE, acquire ControlFileLock spinlock while moving file into
 * place.  This should be TRUE except during bootstrap log creation.  The
 * caller must *not* hold the spinlock at call.
 *
 * Returns FD of opened file.
 */
static int
XLogFileInit(uint32 log, uint32 seg,
			 bool *use_existent, bool use_lock)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	char		targpath[MAXPGPATH];
	char		zbuffer[BLCKSZ];
	uint32		targlog,
				targseg;
	int			fd;
	int			nbytes;

	XLogFileName(path, log, seg);

	/*
	 * Try to use existent file (checkpoint maker may have created it already)
	 */
	if (*use_existent)
	{
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY | XLOG_SYNC_BIT,
						   S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			if (errno != ENOENT)
				elog(STOP, "InitOpen(logfile %u seg %u) failed: %m",
					 log, seg);
		}
		else
			return(fd);
	}

	/*
	 * Initialize an empty (all zeroes) segment.  NOTE: it is possible that
	 * another process is doing the same thing.  If so, we will end up
	 * pre-creating an extra log segment.  That seems OK, and better than
	 * holding the spinlock throughout this lengthy process.
	 */
	snprintf(tmppath, MAXPGPATH, "%s%cxlogtemp.%d",
			 XLogDir, SEP_CHAR, (int) getpid());

	unlink(tmppath);

	/* do not use XLOG_SYNC_BIT here --- want to fsync only at end of fill */
	fd = BasicOpenFile(tmppath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitCreate(%s) failed: %m", tmppath);

	/*
	 * Zero-fill the file.  We have to do this the hard way to ensure that
	 * all the file space has really been allocated --- on platforms that
	 * allow "holes" in files, just seeking to the end doesn't allocate
	 * intermediate space.  This way, we know that we have all the space
	 * and (after the fsync below) that all the indirect blocks are down
	 * on disk.  Therefore, fdatasync(2) or O_DSYNC will be sufficient to
	 * sync future writes to the log file.
	 */
	MemSet(zbuffer, 0, sizeof(zbuffer));
	for (nbytes = 0; nbytes < XLogSegSize; nbytes += sizeof(zbuffer))
	{
		if ((int) write(fd, zbuffer, sizeof(zbuffer)) != (int) sizeof(zbuffer))
		{
			int		save_errno = errno;

			/* If we fail to make the file, delete it to release disk space */
			unlink(tmppath);
			errno = save_errno;

			elog(STOP, "ZeroFill(%s) failed: %m", tmppath);
		}
	}

	if (pg_fsync(fd) != 0)
		elog(STOP, "fsync(%s) failed: %m", tmppath);

	close(fd);

	/*
	 * Now move the segment into place with its final name.  We want to be
	 * sure that only one process does this at a time.
	 */
	if (use_lock)
		SpinAcquire(ControlFileLockId);

	/*
	 * If caller didn't want to use a pre-existing file, get rid of any
	 * pre-existing file.  Otherwise, cope with possibility that someone
	 * else has created the file while we were filling ours: if so, use
	 * ours to pre-create a future log segment.
	 */
	targlog = log;
	targseg = seg;
	strcpy(targpath, path);

	if (! *use_existent)
	{
		unlink(targpath);
	}
	else
	{
		while ((fd = BasicOpenFile(targpath, O_RDWR | PG_BINARY,
								   S_IRUSR | S_IWUSR)) >= 0)
		{
			close(fd);
			NextLogSeg(targlog, targseg);
			XLogFileName(targpath, targlog, targseg);
		}
	}

	/*
	 * Prefer link() to rename() here just to be really sure that we don't
	 * overwrite an existing logfile.  However, there shouldn't be one, so
	 * rename() is an acceptable substitute except for the truly paranoid.
	 */
#ifndef __BEOS__
	if (link(tmppath, targpath) < 0)
		elog(STOP, "InitRelink(logfile %u seg %u) failed: %m",
			 targlog, targseg);
	unlink(tmppath);
#else
	if (rename(tmppath, targpath) < 0)
		elog(STOP, "InitRelink(logfile %u seg %u) failed: %m",
			 targlog, targseg);
#endif

	if (use_lock)
		SpinRelease(ControlFileLockId);

	/* Set flag to tell caller there was no existent file */
	*use_existent = false;

	/* Now open original target segment (might not be file I just made) */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | XLOG_SYNC_BIT,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitReopen(logfile %u seg %u) failed: %m",
			 log, seg);

	return (fd);
}

/*
 * Open a pre-existing logfile segment.
 */
static int
XLogFileOpen(uint32 log, uint32 seg, bool econt)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | XLOG_SYNC_BIT,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (econt && errno == ENOENT)
		{
			elog(LOG, "open(logfile %u seg %u) failed: %m",
				 log, seg);
			return (fd);
		}
		elog(STOP, "open(logfile %u seg %u) failed: %m",
			 log, seg);
	}

	return (fd);
}

/*
 * Preallocate log files beyond the specified log endpoint, according to
 * the XLOGfile user parameter.
 */
static void
PreallocXlogFiles(XLogRecPtr endptr)
{
	uint32		_logId;
	uint32		_logSeg;
	int			lf;
	bool		use_existent;
	int			i;

	XLByteToPrevSeg(endptr, _logId, _logSeg);
	if (XLOGfiles > 0)
	{
		for (i = 1; i <= XLOGfiles; i++)
		{
			NextLogSeg(_logId, _logSeg);
			use_existent = true;
			lf = XLogFileInit(_logId, _logSeg, &use_existent, true);
			close(lf);
		}
	}
	else if ((endptr.xrecoff - 1) % XLogSegSize >=
			 (uint32) (0.75 * XLogSegSize))
	{
		NextLogSeg(_logId, _logSeg);
		use_existent = true;
		lf = XLogFileInit(_logId, _logSeg, &use_existent, true);
		close(lf);
	}
}

/*
 * Remove or move offline all log files older or equal to passed log/seg#
 */
static void
MoveOfflineLogs(uint32 log, uint32 seg)
{
	DIR			   *xldir;
	struct dirent  *xlde;
	char			lastoff[32];
	char			path[MAXPGPATH];

	Assert(XLOG_archive_dir[0] == 0);	/* ! implemented yet */

	xldir = opendir(XLogDir);
	if (xldir == NULL)
		elog(STOP, "MoveOfflineLogs: cannot open xlog dir: %m");

	sprintf(lastoff, "%08X%08X", log, seg);

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 16 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 16 &&
			strcmp(xlde->d_name, lastoff) <= 0)
		{
			elog(LOG, "MoveOfflineLogs: %s %s", (XLOG_archive_dir[0]) ? 
				 "archive" : "remove", xlde->d_name);
			sprintf(path, "%s%c%s",	XLogDir, SEP_CHAR, xlde->d_name);
			if (XLOG_archive_dir[0] == 0)
				unlink(path);
		}
		errno = 0;
	}
	if (errno)
		elog(STOP, "MoveOfflineLogs: cannot read xlog dir: %m");
	closedir(xldir);
}

/*
 * Restore the backup blocks present in an XLOG record, if any.
 *
 * We assume all of the record has been read into memory at *record.
 */
static void
RestoreBkpBlocks(XLogRecord *record, XLogRecPtr lsn)
{
	Relation	reln;
	Buffer		buffer;
	Page		page;
	BkpBlock	bkpb;
	char	   *blk;
	int			i;

	blk = (char*)XLogRecGetData(record) + record->xl_len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;

		memcpy((char*)&bkpb, blk, sizeof(BkpBlock));
		blk += sizeof(BkpBlock);

		reln = XLogOpenRelation(true, record->xl_rmid, bkpb.node);

		if (reln)
		{
			buffer = XLogReadBuffer(true, reln, bkpb.block);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);
				memcpy((char*)page, blk, BLCKSZ);
				PageSetLSN(page, lsn);
				PageSetSUI(page, ThisStartUpID);
				UnlockAndWriteBuffer(buffer);
			}
		}

		blk += BLCKSZ;
	}
}

/*
 * CRC-check an XLOG record.  We do not believe the contents of an XLOG
 * record (other than to the minimal extent of computing the amount of
 * data to read in) until we've checked the CRCs.
 *
 * We assume all of the record has been read into memory at *record.
 */
static bool
RecordIsValid(XLogRecord *record, XLogRecPtr recptr, int emode)
{
	crc64		crc;
	crc64		cbuf;
	int			i;
	uint32		len = record->xl_len;
	char	   *blk;

	/* Check CRC of rmgr data and record header */
	INIT_CRC64(crc);
	COMP_CRC64(crc, XLogRecGetData(record), len);
	COMP_CRC64(crc, (char*) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(record->xl_crc, crc))
	{
		elog(emode, "ReadRecord: bad rmgr data CRC in record at %u/%u",
			 recptr.xlogid, recptr.xrecoff);
		return(false);
	}

	/* Check CRCs of backup blocks, if any */
	blk = (char*)XLogRecGetData(record) + len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;

		INIT_CRC64(crc);
		COMP_CRC64(crc, blk + sizeof(BkpBlock), BLCKSZ);
		COMP_CRC64(crc, blk + sizeof(crc64),
				   sizeof(BkpBlock) - sizeof(crc64));
		FIN_CRC64(crc);
		memcpy((char*)&cbuf, blk, sizeof(crc64)); /* don't assume alignment */

		if (!EQ_CRC64(cbuf, crc))
		{
			elog(emode, "ReadRecord: bad bkp block %d CRC in record at %u/%u",
				 i + 1, recptr.xlogid, recptr.xrecoff);
			return(false);
		}
		blk += sizeof(BkpBlock) + BLCKSZ;
	}

	return(true);
}

/*
 * Attempt to read an XLOG record.
 *
 * If RecPtr is not NULL, try to read a record at that position.  Otherwise
 * try to read a record just after the last one previously read.
 *
 * If no valid record is available, returns NULL, or fails if emode is STOP.
 * (emode must be either STOP or LOG.)
 *
 * buffer is a workspace at least _INTL_MAXLOGRECSZ bytes long.  It is needed
 * to reassemble a record that crosses block boundaries.  Note that on
 * successful return, the returned record pointer always points at buffer.
 */
static XLogRecord *
ReadRecord(XLogRecPtr *RecPtr, int emode, char *buffer)
{
	XLogRecord *record;
	XLogRecPtr	tmpRecPtr = EndRecPtr;
	uint32		len,
				total_len;
	uint32		targetPageOff;
	unsigned	i;
	bool		nextmode = false;

	if (readBuf == NULL)
	{
		/*
		 * First time through, permanently allocate readBuf.  We do it
		 * this way, rather than just making a static array, for two
		 * reasons: (1) no need to waste the storage in most instantiations
		 * of the backend; (2) a static char array isn't guaranteed to
		 * have any particular alignment, whereas malloc() will provide
		 * MAXALIGN'd storage.
		 */
		readBuf = (char *) malloc(BLCKSZ);
		Assert(readBuf != NULL);
	}

	if (RecPtr == NULL)
	{
		RecPtr = &tmpRecPtr;
		nextmode = true;
		/* fast case if next record is on same page */
		if (nextRecord != NULL)
		{
			record = nextRecord;
			goto got_record;
		}
		/* align old recptr to next page */
		if (tmpRecPtr.xrecoff % BLCKSZ != 0)
			tmpRecPtr.xrecoff += (BLCKSZ - tmpRecPtr.xrecoff % BLCKSZ);
		if (tmpRecPtr.xrecoff >= XLogFileSize)
		{
			(tmpRecPtr.xlogid)++;
			tmpRecPtr.xrecoff = 0;
		}
		tmpRecPtr.xrecoff += SizeOfXLogPHD;
	}
	else if (!XRecOffIsValid(RecPtr->xrecoff))
		elog(STOP, "ReadRecord: invalid record offset at (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);

	if (readFile >= 0 && !XLByteInSeg(*RecPtr, readId, readSeg))
	{
		close(readFile);
		readFile = -1;
	}
	XLByteToSeg(*RecPtr, readId, readSeg);
	if (readFile < 0)
	{
		readFile = XLogFileOpen(readId, readSeg, (emode == LOG));
		if (readFile < 0)
			goto next_record_is_invalid;
		readOff = (uint32) (-1); /* force read to occur below */
	}

	targetPageOff = ((RecPtr->xrecoff % XLogSegSize) / BLCKSZ) * BLCKSZ;
	if (readOff != targetPageOff)
	{
		readOff = targetPageOff;
		if (lseek(readFile, (off_t) readOff, SEEK_SET) < 0)
		{
			elog(emode, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
			goto next_record_is_invalid;
		}
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
		{
			elog(emode, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
			goto next_record_is_invalid;
		}
		if (!ValidXLOGHeader((XLogPageHeader) readBuf, emode, nextmode))
			goto next_record_is_invalid;
	}
	if ((((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		RecPtr->xrecoff % BLCKSZ == SizeOfXLogPHD)
	{
		elog(emode, "ReadRecord: contrecord is requested by (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	record = (XLogRecord *) ((char *) readBuf + RecPtr->xrecoff % BLCKSZ);

got_record:;
	/*
	 * Currently, xl_len == 0 must be bad data, but that might not be
	 * true forever.  See note in XLogInsert.
	 */
	if (record->xl_len == 0)
	{
		elog(emode, "ReadRecord: record with zero len at (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	/*
	 * Compute total length of record including any appended backup blocks.
	 */
	total_len = SizeOfXLogRecord + record->xl_len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;
		total_len += sizeof(BkpBlock) + BLCKSZ;
	}
	/*
	 * Make sure it will fit in buffer (currently, it is mechanically
	 * impossible for this test to fail, but it seems like a good idea
	 * anyway).
	 */
	if (total_len > _INTL_MAXLOGRECSZ)
	{
		elog(emode, "ReadRecord: too long record len %u at (%u, %u)",
			 total_len, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		elog(emode, "ReadRecord: invalid resource manager id %u at (%u, %u)",
			 record->xl_rmid, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	nextRecord = NULL;
	len = BLCKSZ - RecPtr->xrecoff % BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		XLogContRecord *contrecord;
		uint32			gotlen = len;

		memcpy(buffer, record, len);
		record = (XLogRecord *) buffer;
		buffer += len;
		for (;;)
		{
			readOff += BLCKSZ;
			if (readOff >= XLogSegSize)
			{
				close(readFile);
				readFile = -1;
				NextLogSeg(readId, readSeg);
				readFile = XLogFileOpen(readId, readSeg, (emode == LOG));
				if (readFile < 0)
					goto next_record_is_invalid;
				readOff = 0;
			}
			if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			{
				elog(emode, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			if (!ValidXLOGHeader((XLogPageHeader) readBuf, emode, true))
				goto next_record_is_invalid;
			if (!(((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				elog(emode, "ReadRecord: there is no ContRecord flag in logfile %u seg %u off %u",
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			contrecord = (XLogContRecord *) ((char *) readBuf + SizeOfXLogPHD);
			if (contrecord->xl_rem_len == 0 || 
				total_len != (contrecord->xl_rem_len + gotlen))
			{
				elog(emode, "ReadRecord: invalid cont-record len %u in logfile %u seg %u off %u",
					 contrecord->xl_rem_len, readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			len = BLCKSZ - SizeOfXLogPHD - SizeOfXLogContRecord;
			if (contrecord->xl_rem_len > len)
			{
				memcpy(buffer, (char *)contrecord + SizeOfXLogContRecord, len);
				gotlen += len;
				buffer += len;
				continue;
			}
			memcpy(buffer, (char *) contrecord + SizeOfXLogContRecord,
				   contrecord->xl_rem_len);
			break;
		}
		if (!RecordIsValid(record, *RecPtr, emode))
			goto next_record_is_invalid;
		if (BLCKSZ - SizeOfXLogRecord >= SizeOfXLogPHD +
			SizeOfXLogContRecord + MAXALIGN(contrecord->xl_rem_len))
		{
			nextRecord = (XLogRecord *) ((char *) contrecord + 
				SizeOfXLogContRecord + MAXALIGN(contrecord->xl_rem_len));
		}
		EndRecPtr.xlogid = readId;
		EndRecPtr.xrecoff = readSeg * XLogSegSize + readOff +
			SizeOfXLogPHD + SizeOfXLogContRecord + 
			MAXALIGN(contrecord->xl_rem_len);
		ReadRecPtr = *RecPtr;
		return record;
	}

	/* Record does not cross a page boundary */
	if (!RecordIsValid(record, *RecPtr, emode))
		goto next_record_is_invalid;
	if (BLCKSZ - SizeOfXLogRecord >= RecPtr->xrecoff % BLCKSZ +
		MAXALIGN(total_len))
		nextRecord = (XLogRecord *) ((char *) record + MAXALIGN(total_len));
	EndRecPtr.xlogid = RecPtr->xlogid;
	EndRecPtr.xrecoff = RecPtr->xrecoff + MAXALIGN(total_len);
	ReadRecPtr = *RecPtr;
	memcpy(buffer, record, total_len);
	return (XLogRecord *) buffer;

next_record_is_invalid:;
	close(readFile);
	readFile = -1;
	nextRecord = NULL;
	return NULL;
}

/*
 * Check whether the xlog header of a page just read in looks valid.
 *
 * This is just a convenience subroutine to avoid duplicated code in
 * ReadRecord.  It's not intended for use from anywhere else.
 */
static bool
ValidXLOGHeader(XLogPageHeader hdr, int emode, bool checkSUI)
{
	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		elog(emode, "ReadRecord: invalid magic number %04X in logfile %u seg %u off %u",
			 hdr->xlp_magic, readId, readSeg, readOff);
		return false;
	}
	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		elog(emode, "ReadRecord: invalid info bits %04X in logfile %u seg %u off %u",
			 hdr->xlp_info, readId, readSeg, readOff);
		return false;
	}
	/*
	 * We disbelieve a SUI less than the previous page's SUI, or more
	 * than a few counts greater.  In theory as many as 512 shutdown
	 * checkpoint records could appear on a 32K-sized xlog page, so
	 * that's the most differential there could legitimately be.
	 *
	 * Note this check can only be applied when we are reading the next page
	 * in sequence, so ReadRecord passes a flag indicating whether to check.
	 */
	if (checkSUI)
	{
		if (hdr->xlp_sui < lastReadSUI ||
			hdr->xlp_sui > lastReadSUI + 512)
		{
			elog(emode, "ReadRecord: out-of-sequence SUI %u (after %u) in logfile %u seg %u off %u",
				 hdr->xlp_sui, lastReadSUI, readId, readSeg, readOff);
			return false;
		}
	}
	lastReadSUI = hdr->xlp_sui;
	return true;
}

/*
 * I/O routines for pg_control
 *
 * *ControlFile is a buffer in shared memory that holds an image of the
 * contents of pg_control.  WriteControlFile() initializes pg_control
 * given a preloaded buffer, ReadControlFile() loads the buffer from
 * the pg_control file (during postmaster or standalone-backend startup),
 * and UpdateControlFile() rewrites pg_control after we modify xlog state.
 *
 * For simplicity, WriteControlFile() initializes the fields of pg_control
 * that are related to checking backend/database compatibility, and
 * ReadControlFile() verifies they are correct.  We could split out the
 * I/O and compatibility-check functions, but there seems no need currently.
 */

void
XLOGPathInit(void)
{
	/* Init XLOG file paths */
	snprintf(XLogDir, MAXPGPATH, "%s%cpg_xlog", DataDir, SEP_CHAR);
	snprintf(ControlFilePath, MAXPGPATH, "%s%cglobal%cpg_control",
			 DataDir, SEP_CHAR, SEP_CHAR);
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ];	/* need not be aligned */
#ifdef USE_LOCALE
	char	   *localeptr;
#endif

	/*
	 * Initialize version and compatibility-check fields
	 */
	ControlFile->pg_control_version = PG_CONTROL_VERSION;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;
	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;
#ifdef USE_LOCALE
	localeptr = setlocale(LC_COLLATE, NULL);
	if (!localeptr)
		elog(STOP, "Invalid LC_COLLATE setting");
	StrNCpy(ControlFile->lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, NULL);
	if (!localeptr)
		elog(STOP, "Invalid LC_CTYPE setting");
	StrNCpy(ControlFile->lc_ctype, localeptr, LOCALE_NAME_BUFLEN);
	/*
	 * Issue warning notice if initdb'ing in a locale that will not permit
	 * LIKE index optimization.  This is not a clean place to do it, but
	 * I don't see a better place either...
	 */
	if (!locale_is_like_safe())
		elog(NOTICE, "Initializing database with %s collation order."
			 "\n\tThis locale setting will prevent use of index optimization for"
			 "\n\tLIKE and regexp searches.  If you are concerned about speed of"
			 "\n\tsuch queries, you may wish to set LC_COLLATE to \"C\" and"
			 "\n\tre-initdb.  For more information see the Administrator's Guide.",
			 ControlFile->lc_collate);
#else
	strcpy(ControlFile->lc_collate, "C");
	strcpy(ControlFile->lc_ctype, "C");
#endif

	/* Contents are protected with a CRC */
	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc, 
			   (char*) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile->crc);

	/*
	 * We write out BLCKSZ bytes into pg_control, zero-padding the
	 * excess over sizeof(ControlFileData).  This reduces the odds
	 * of premature-EOF errors when reading pg_control.  We'll still
	 * fail when we check the contents of the file, but hopefully with
	 * a more specific error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > BLCKSZ)
		elog(STOP, "sizeof(ControlFileData) is too large ... fix xlog.c");

	memset(buffer, 0, BLCKSZ);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	fd = BasicOpenFile(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "WriteControlFile failed to create control file (%s): %m",
			 ControlFilePath);

	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "WriteControlFile failed to write control file: %m");

	if (pg_fsync(fd) != 0)
		elog(STOP, "WriteControlFile failed to fsync control file: %m");

	close(fd);
}

static void
ReadControlFile(void)
{
	crc64		crc;
	int			fd;

	/*
	 * Read data...
	 */
	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "open(\"%s\") failed: %m", ControlFilePath);

	if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		elog(STOP, "read(\"%s\") failed: %m", ControlFilePath);

	close(fd);

	/*
	 * Check for expected pg_control format version.  If this is wrong,
	 * the CRC check will likely fail because we'll be checking the wrong
	 * number of bytes.  Complaining about wrong version will probably be
	 * more enlightening than complaining about wrong CRC.
	 */
	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		elog(STOP, "database was initialized with PG_CONTROL_VERSION %d,\n\tbut the backend was compiled with PG_CONTROL_VERSION %d.\n\tlooks like you need to initdb.",
			 ControlFile->pg_control_version, PG_CONTROL_VERSION);

	/* Now check the CRC. */
	INIT_CRC64(crc);
	COMP_CRC64(crc, 
			   (char*) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(crc, ControlFile->crc))
		elog(STOP, "Invalid CRC in control file");

	/*
	 * Do compatibility checking immediately.  We do this here for 2 reasons:
	 *
	 * (1) if the database isn't compatible with the backend executable,
	 * we want to abort before we can possibly do any damage;
	 *
	 * (2) this code is executed in the postmaster, so the setlocale() will
	 * propagate to forked backends, which aren't going to read this file
	 * for themselves.  (These locale settings are considered critical
	 * compatibility items because they can affect sort order of indexes.)
	 */
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		elog(STOP, "database was initialized with CATALOG_VERSION_NO %d,\n\tbut the backend was compiled with CATALOG_VERSION_NO %d.\n\tlooks like you need to initdb.",
			 ControlFile->catalog_version_no, CATALOG_VERSION_NO);
	if (ControlFile->blcksz != BLCKSZ)
		elog(STOP, "database was initialized with BLCKSZ %d,\n\tbut the backend was compiled with BLCKSZ %d.\n\tlooks like you need to initdb.",
			 ControlFile->blcksz, BLCKSZ);
	if (ControlFile->relseg_size != RELSEG_SIZE)
		elog(STOP, "database was initialized with RELSEG_SIZE %d,\n\tbut the backend was compiled with RELSEG_SIZE %d.\n\tlooks like you need to initdb.",
			 ControlFile->relseg_size, RELSEG_SIZE);
#ifdef USE_LOCALE
	if (setlocale(LC_COLLATE, ControlFile->lc_collate) == NULL)
		elog(STOP, "database was initialized with LC_COLLATE '%s',\n\twhich is not recognized by setlocale().\n\tlooks like you need to initdb.",
			 ControlFile->lc_collate);
	if (setlocale(LC_CTYPE, ControlFile->lc_ctype) == NULL)
		elog(STOP, "database was initialized with LC_CTYPE '%s',\n\twhich is not recognized by setlocale().\n\tlooks like you need to initdb.",
			 ControlFile->lc_ctype);
#else
	if (strcmp(ControlFile->lc_collate, "C") != 0 ||
		strcmp(ControlFile->lc_ctype, "C") != 0)
		elog(STOP, "database was initialized with LC_COLLATE '%s' and LC_CTYPE '%s',\n\tbut the backend was compiled without locale support.\n\tlooks like you need to initdb or recompile.",
			 ControlFile->lc_collate, ControlFile->lc_ctype);
#endif
}

void
UpdateControlFile(void)
{
	int			fd;

	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc, 
			   (char*) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile->crc);

	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "open(\"%s\") failed: %m", ControlFilePath);

	if (write(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		elog(STOP, "write(cntlfile) failed: %m");

	if (pg_fsync(fd) != 0)
		elog(STOP, "fsync(cntlfile) failed: %m");

	close(fd);
}

/*
 * Initialization of shared memory for XLOG
 */

int
XLOGShmemSize(void)
{
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	return MAXALIGN(sizeof(XLogCtlData) + sizeof(XLogRecPtr) * XLOGbuffers)
		+ BLCKSZ * XLOGbuffers +
		MAXALIGN(sizeof(ControlFileData));
}

void
XLOGShmemInit(void)
{
	bool		found;

	/* this must agree with space requested by XLOGShmemSize() */
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	XLogCtl = (XLogCtlData *)
		ShmemInitStruct("XLOG Ctl",
						MAXALIGN(sizeof(XLogCtlData) +
								 sizeof(XLogRecPtr) * XLOGbuffers)
						+ BLCKSZ * XLOGbuffers,
						&found);
	Assert(!found);
	ControlFile = (ControlFileData *)
		ShmemInitStruct("Control File", sizeof(ControlFileData), &found);
	Assert(!found);

	memset(XLogCtl, 0, sizeof(XLogCtlData));
	/*
	 * Since XLogCtlData contains XLogRecPtr fields, its sizeof should be
	 * a multiple of the alignment for same, so no extra alignment padding
	 * is needed here.
	 */
	XLogCtl->xlblocks = (XLogRecPtr *)
		(((char *) XLogCtl) + sizeof(XLogCtlData));
	memset(XLogCtl->xlblocks, 0, sizeof(XLogRecPtr) * XLOGbuffers);
	/*
	 * Here, on the other hand, we must MAXALIGN to ensure the page buffers
	 * have worst-case alignment.
	 */
	XLogCtl->pages =
		((char *) XLogCtl) + MAXALIGN(sizeof(XLogCtlData) +
									  sizeof(XLogRecPtr) * XLOGbuffers);
	memset(XLogCtl->pages, 0, BLCKSZ * XLOGbuffers);

	/*
	 * Do basic initialization of XLogCtl shared data.
	 * (StartupXLOG will fill in additional info.)
	 */
	XLogCtl->XLogCacheByte = BLCKSZ * XLOGbuffers;
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	XLogCtl->Insert.currpage = (XLogPageHeader) (XLogCtl->pages);
	S_INIT_LOCK(&(XLogCtl->insert_lck));
	S_INIT_LOCK(&(XLogCtl->info_lck));
	S_INIT_LOCK(&(XLogCtl->logwrt_lck));
	S_INIT_LOCK(&(XLogCtl->chkp_lck));

	/*
	 * If we are not in bootstrap mode, pg_control should already exist.
	 * Read and validate it immediately (see comments in ReadControlFile()
	 * for the reasons why).
	 */
	if (!IsBootstrapProcessingMode())
		ReadControlFile();
}

/*
 * This func must be called ONCE on system install.  It creates pg_control
 * and the initial XLOG segment.
 */
void
BootStrapXLOG(void)
{
	CheckPoint	checkPoint;
	char	   *buffer;
	XLogPageHeader page;
	XLogRecord *record;
	bool        use_existent;
	crc64		crc;

	/* Use malloc() to ensure buffer is MAXALIGNED */
	buffer = (char *) malloc(BLCKSZ);
	page = (XLogPageHeader) buffer;

	checkPoint.redo.xlogid = 0;
	checkPoint.redo.xrecoff = SizeOfXLogPHD;
	checkPoint.undo = checkPoint.redo;
	checkPoint.ThisStartUpID = 0;
	checkPoint.nextXid = FirstTransactionId;
	checkPoint.nextOid = BootstrapObjectIdData;
	checkPoint.time = time(NULL);

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->xidCount = 0;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	page->xlp_sui = checkPoint.ThisStartUpID;
	record = (XLogRecord *) ((char *) page + SizeOfXLogPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xact_prev = record->xl_prev;
	record->xl_xid = InvalidTransactionId;
	record->xl_len = sizeof(checkPoint);
	record->xl_info = XLOG_CHECKPOINT_SHUTDOWN;
	record->xl_rmid = RM_XLOG_ID;
	memcpy(XLogRecGetData(record), &checkPoint, sizeof(checkPoint));

	INIT_CRC64(crc);
	COMP_CRC64(crc, &checkPoint, sizeof(checkPoint));
	COMP_CRC64(crc, (char*) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);
	record->xl_crc = crc;

	use_existent = false;
	openLogFile = XLogFileInit(0, 0, &use_existent, false);

	if (write(openLogFile, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "BootStrapXLOG failed to write logfile: %m");

	if (pg_fsync(openLogFile) != 0)
		elog(STOP, "BootStrapXLOG failed to fsync logfile: %m");

	close(openLogFile);
	openLogFile = -1;

	memset(ControlFile, 0, sizeof(ControlFileData));
	/* Initialize pg_control status fields */
	ControlFile->state = DB_SHUTDOWNED;
	ControlFile->time = checkPoint.time;
	ControlFile->logId = 0;
	ControlFile->logSeg = 1;
	ControlFile->checkPoint = checkPoint.redo;
	ControlFile->checkPointCopy = checkPoint;
	/* some additional ControlFile fields are set in WriteControlFile() */

	WriteControlFile();
}

static char *
str_time(time_t tnow)
{
	static char buf[32];

	strftime(buf, sizeof(buf),
			 "%Y-%m-%d %H:%M:%S %Z",
			 localtime(&tnow));

	return buf;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup
 */
void
StartupXLOG(void)
{
	XLogCtlInsert *Insert;
	CheckPoint	checkPoint;
	bool		wasShutdown;
	XLogRecPtr	RecPtr,
				LastRec,
				checkPointLoc,
				EndOfLog;
	XLogRecord *record;
	char	   *buffer;

	/* Use malloc() to ensure record buffer is MAXALIGNED */
	buffer = (char *) malloc(_INTL_MAXLOGRECSZ);

	CritSectionCount++;

	/*
	 * Read control file and check XLOG status looks valid.
	 *
	 * Note: in most control paths, *ControlFile is already valid and we
	 * need not do ReadControlFile() here, but might as well do it to be sure.
	 */
	ReadControlFile();

	if (ControlFile->logSeg == 0 ||
		ControlFile->time <= 0 ||
		ControlFile->state < DB_SHUTDOWNED ||
		ControlFile->state > DB_IN_PRODUCTION ||
		!XRecOffIsValid(ControlFile->checkPoint.xrecoff))
		elog(STOP, "control file context is broken");

	if (ControlFile->state == DB_SHUTDOWNED)
		elog(LOG, "database system was shut down at %s",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_SHUTDOWNING)
		elog(LOG, "database system shutdown was interrupted at %s",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_IN_RECOVERY)
		elog(LOG, "database system was interrupted being in recovery at %s\n"
			 "\tThis propably means that some data blocks are corrupted\n"
			 "\tand you will have to use last backup for recovery.",
			 str_time(ControlFile->time));
	else if (ControlFile->state == DB_IN_PRODUCTION)
		elog(LOG, "database system was interrupted at %s",
			 str_time(ControlFile->time));

	/*
	 * Get the last valid checkpoint record.  If the latest one according
	 * to pg_control is broken, try the next-to-last one.
	 */
	record = ReadCheckpointRecord(ControlFile->checkPoint,
								  "primary", buffer);
	if (record != NULL)
	{
		checkPointLoc = ControlFile->checkPoint;
		elog(LOG, "CheckPoint record at (%u, %u)",
			 checkPointLoc.xlogid, checkPointLoc.xrecoff);
	}
	else
	{
		record = ReadCheckpointRecord(ControlFile->prevCheckPoint,
									  "secondary", buffer);
		if (record != NULL)
		{
			checkPointLoc = ControlFile->prevCheckPoint;
			elog(LOG, "Using previous CheckPoint record at (%u, %u)",
				 checkPointLoc.xlogid, checkPointLoc.xrecoff);
			InRecovery = true;	/* force recovery even if SHUTDOWNED */
		}
		else
		{
			elog(STOP, "Unable to locate a valid CheckPoint record");
		}
	}
	LastRec = RecPtr = checkPointLoc;
	memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
	wasShutdown = (record->xl_info == XLOG_CHECKPOINT_SHUTDOWN);

	elog(LOG, "Redo record at (%u, %u); Undo record at (%u, %u); Shutdown %s",
		 checkPoint.redo.xlogid, checkPoint.redo.xrecoff,
		 checkPoint.undo.xlogid, checkPoint.undo.xrecoff,
		 wasShutdown ? "TRUE" : "FALSE");
	elog(LOG, "NextTransactionId: %u; NextOid: %u",
		 checkPoint.nextXid, checkPoint.nextOid);
	if (checkPoint.nextXid < FirstTransactionId ||
		checkPoint.nextOid < BootstrapObjectIdData)
		elog(STOP, "Invalid NextTransactionId/NextOid");

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->xidCount = 0;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	ThisStartUpID = checkPoint.ThisStartUpID;
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = 
		XLogCtl->RedoRecPtr = checkPoint.redo;

	if (XLByteLT(RecPtr, checkPoint.redo))
		elog(STOP, "Invalid redo in checkPoint record");
	if (checkPoint.undo.xrecoff == 0)
		checkPoint.undo = RecPtr;

	if (XLByteLT(checkPoint.undo, RecPtr) || 
		XLByteLT(checkPoint.redo, RecPtr))
	{
		if (wasShutdown)
			elog(STOP, "Invalid Redo/Undo record in shutdown checkpoint");
		InRecovery = true;
	}
	else if (ControlFile->state != DB_SHUTDOWNED)
	{
		InRecovery = true;
	}

	/* REDO */
	if (InRecovery)
	{
		elog(LOG, "database system was not properly shut down; "
			 "automatic recovery in progress...");
		ControlFile->state = DB_IN_RECOVERY;
		ControlFile->time = time(NULL);
		UpdateControlFile();

		XLogOpenLogRelation();	/* open pg_log */
		XLogInitRelationCache();

		/* Is REDO required ? */
		if (XLByteLT(checkPoint.redo, RecPtr))
			record = ReadRecord(&(checkPoint.redo), STOP, buffer);
		else	/* read past CheckPoint record */
			record = ReadRecord(NULL, LOG, buffer);

		if (record != NULL)
		{
			InRedo = true;
			elog(LOG, "redo starts at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			do
			{
				if (record->xl_xid >= ShmemVariableCache->nextXid)
				{
					/* This probably shouldn't happen... */
					ShmemVariableCache->nextXid = record->xl_xid + 1;
					ShmemVariableCache->xidCount = 0;
				}
				if (XLOG_DEBUG)
				{
					char	buf[8192];

					sprintf(buf, "REDO @ %u/%u; LSN %u/%u: ", 
						ReadRecPtr.xlogid, ReadRecPtr.xrecoff,
						EndRecPtr.xlogid, EndRecPtr.xrecoff);
					xlog_outrec(buf, record);
					strcat(buf, " - ");
					RmgrTable[record->xl_rmid].rm_desc(buf, 
						record->xl_info, XLogRecGetData(record));
					fprintf(stderr, "%s\n", buf);
				}

				if (record->xl_info & XLR_BKP_BLOCK_MASK)
					RestoreBkpBlocks(record, EndRecPtr);

				RmgrTable[record->xl_rmid].rm_redo(EndRecPtr, record);
				record = ReadRecord(NULL, LOG, buffer);
			} while (record != NULL);
			elog(LOG, "redo done at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			LastRec = ReadRecPtr;
			InRedo = false;
		}
		else
			elog(LOG, "redo is not required");
	}

	/*
	 * Init xlog buffer cache using the block containing the last valid
	 * record from the previous incarnation.
	 */
	record = ReadRecord(&LastRec, STOP, buffer);
	EndOfLog = EndRecPtr;
	XLByteToPrevSeg(EndOfLog, openLogId, openLogSeg);
	openLogFile = XLogFileOpen(openLogId, openLogSeg, false);
	openLogOff = 0;
	ControlFile->logId = openLogId;
	ControlFile->logSeg = openLogSeg + 1;
	XLogCtl->xlblocks[0].xlogid = openLogId;
	XLogCtl->xlblocks[0].xrecoff =
		((EndOfLog.xrecoff - 1) / BLCKSZ + 1) * BLCKSZ;
	Insert = &XLogCtl->Insert;
	/* Tricky point here: readBuf contains the *last* block that the LastRec
	 * record spans, not the one it starts in, which is what we want.
	 */
	Assert(readOff == (XLogCtl->xlblocks[0].xrecoff - BLCKSZ) % XLogSegSize);
	memcpy((char *) Insert->currpage, readBuf, BLCKSZ);
	Insert->currpos = (char *) Insert->currpage +
		(EndOfLog.xrecoff + BLCKSZ - XLogCtl->xlblocks[0].xrecoff);
	/* Make sure rest of page is zero */
	memset(Insert->currpos, 0, INSERT_FREESPACE(Insert));
	Insert->PrevRecord = LastRec;

	LogwrtResult.Write = LogwrtResult.Flush = EndOfLog;

	XLogCtl->Write.LogwrtResult = LogwrtResult;
	Insert->LogwrtResult = LogwrtResult;
	XLogCtl->LogwrtResult = LogwrtResult;

	XLogCtl->LogwrtRqst.Write = EndOfLog;
	XLogCtl->LogwrtRqst.Flush = EndOfLog;

#ifdef NOT_USED
	/* UNDO */
	if (InRecovery)
	{
		RecPtr = ReadRecPtr;
		if (XLByteLT(checkPoint.undo, RecPtr))
		{
			elog(LOG, "undo starts at (%u, %u)",
				 RecPtr.xlogid, RecPtr.xrecoff);
			do
			{
				record = ReadRecord(&RecPtr, STOP, buffer);
				if (TransactionIdIsValid(record->xl_xid) &&
					!TransactionIdDidCommit(record->xl_xid))
					RmgrTable[record->xl_rmid].rm_undo(EndRecPtr, record);
				RecPtr = record->xl_prev;
			} while (XLByteLE(checkPoint.undo, RecPtr));
			elog(LOG, "undo done at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
		}
		else
			elog(LOG, "undo is not required");
	}
#endif

	if (InRecovery)
	{
		/*
		 * In case we had to use the secondary checkpoint, make sure that
		 * it will still be shown as the secondary checkpoint after this
		 * CreateCheckPoint operation; we don't want the broken primary
		 * checkpoint to become prevCheckPoint...
		 */
		ControlFile->checkPoint = checkPointLoc;
		CreateCheckPoint(true);
		XLogCloseRelationCache();
	}

	/*
	 * Preallocate additional log files, if wanted.
	 */
	PreallocXlogFiles(EndOfLog);

	InRecovery = false;

	ControlFile->state = DB_IN_PRODUCTION;
	ControlFile->time = time(NULL);
	UpdateControlFile();

	ThisStartUpID++;
	XLogCtl->ThisStartUpID = ThisStartUpID;

	elog(LOG, "database system is in production state");
	CritSectionCount--;

	/* Shut down readFile facility, free space */
	if (readFile >= 0)
	{
		close(readFile);
		readFile = -1;
	}
	if (readBuf)
	{
		free(readBuf);
		readBuf = NULL;
	}

	free(buffer);
}

/* Subroutine to try to fetch and validate a prior checkpoint record */
static XLogRecord *
ReadCheckpointRecord(XLogRecPtr RecPtr,
					 const char *whichChkpt,
					 char *buffer)
{
	XLogRecord *record;

	if (!XRecOffIsValid(RecPtr.xrecoff))
	{
		elog(LOG, "Invalid %s checkPoint link in control file", whichChkpt);
		return NULL;
	}

	record = ReadRecord(&RecPtr, LOG, buffer);

	if (record == NULL)
	{
		elog(LOG, "Invalid %s checkPoint record", whichChkpt);
		return NULL;
	}
	if (record->xl_rmid != RM_XLOG_ID)
	{
		elog(LOG, "Invalid RMID in %s checkPoint record", whichChkpt);
		return NULL;
	}
	if (record->xl_info != XLOG_CHECKPOINT_SHUTDOWN &&
		record->xl_info != XLOG_CHECKPOINT_ONLINE)
	{
		elog(LOG, "Invalid xl_info in %s checkPoint record", whichChkpt);
		return NULL;
	}
	if (record->xl_len != sizeof(CheckPoint))
	{
		elog(LOG, "Invalid length of %s checkPoint record", whichChkpt);
		return NULL;
	}
	return record;
}

/*
 * Postmaster uses this to initialize ThisStartUpID & RedoRecPtr from
 * XLogCtlData located in shmem after successful startup.
 */
void
SetThisStartUpID(void)
{
	ThisStartUpID = XLogCtl->ThisStartUpID;
	RedoRecPtr = XLogCtl->RedoRecPtr;
}

/*
 * CheckPoint process called by postmaster saves copy of new RedoRecPtr
 * in shmem (using SetRedoRecPtr).  When checkpointer completes, postmaster
 * calls GetRedoRecPtr to update its own copy of RedoRecPtr, so that
 * subsequently-spawned backends will start out with a reasonably up-to-date
 * local RedoRecPtr.  Since these operations are not protected by any spinlock
 * and copying an XLogRecPtr isn't atomic, it's unsafe to use either of these
 * routines at other times!
 *
 * Note: once spawned, a backend must update its local RedoRecPtr from
 * XLogCtl->Insert.RedoRecPtr while holding the insert spinlock.  This is
 * done in XLogInsert().
 */
void
SetRedoRecPtr(void)
{
	XLogCtl->RedoRecPtr = RedoRecPtr;
}

void
GetRedoRecPtr(void)
{
	RedoRecPtr = XLogCtl->RedoRecPtr;
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownXLOG(void)
{
	elog(LOG, "shutting down");

	/* suppress in-transaction check in CreateCheckPoint */
	MyLastRecPtr.xrecoff = 0;

	CritSectionCount++;
	CreateDummyCaches();
	CreateCheckPoint(true);
	CritSectionCount--;

	elog(LOG, "database system is shut down");
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CreateCheckPoint(bool shutdown)
{
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogRecData	rdata;
	uint32		freespace;
	uint32		_logId;
	uint32		_logSeg;
	unsigned	spins = 0;

	if (MyLastRecPtr.xrecoff != 0)
		elog(ERROR, "CreateCheckPoint: cannot be called inside transaction block");
 
	START_CRIT_SECTION();

	/* Grab lock, using larger than normal sleep between tries (1 sec) */
	while (TAS(&(XLogCtl->chkp_lck)))
	{
		S_LOCK_SLEEP_INTERVAL(&(XLogCtl->chkp_lck), spins++,
							  CHECKPOINT_LOCK_TIMEOUT, 1000000);
	}

	if (shutdown)
	{
		ControlFile->state = DB_SHUTDOWNING;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}

	memset(&checkPoint, 0, sizeof(checkPoint));
	checkPoint.ThisStartUpID = ThisStartUpID;
	checkPoint.time = time(NULL);

	S_LOCK(&(XLogCtl->insert_lck));

	/*
	 * If this isn't a shutdown, and we have not inserted any XLOG records
	 * since the start of the last checkpoint, skip the checkpoint.  The
	 * idea here is to avoid inserting duplicate checkpoints when the system
	 * is idle.  That wastes log space, and more importantly it exposes us to
	 * possible loss of both current and previous checkpoint records if the
	 * machine crashes just as we're writing the update.  (Perhaps it'd make
	 * even more sense to checkpoint only when the previous checkpoint record
	 * is in a different xlog page?)
	 *
	 * We have to make two tests to determine that nothing has happened since
	 * the start of the last checkpoint: current insertion point must match
	 * the end of the last checkpoint record, and its redo pointer must point
	 * to itself.
	 */
	if (!shutdown)
	{
		XLogRecPtr	curInsert;

		INSERT_RECPTR(curInsert, Insert, Insert->curridx);
		if (curInsert.xlogid == ControlFile->checkPoint.xlogid &&
			curInsert.xrecoff == ControlFile->checkPoint.xrecoff +
			MAXALIGN(SizeOfXLogRecord + sizeof(CheckPoint)) &&
			ControlFile->checkPoint.xlogid ==
			ControlFile->checkPointCopy.redo.xlogid &&
			ControlFile->checkPoint.xrecoff ==
			ControlFile->checkPointCopy.redo.xrecoff)
		{
			S_UNLOCK(&(XLogCtl->insert_lck));
			S_UNLOCK(&(XLogCtl->chkp_lck));
			END_CRIT_SECTION();
			return;
		}
	}

	/*
	 * Compute new REDO record ptr = location of next XLOG record.
	 *
	 * NB: this is NOT necessarily where the checkpoint record itself will
	 * be, since other backends may insert more XLOG records while we're
	 * off doing the buffer flush work.  Those XLOG records are logically
	 * after the checkpoint, even though physically before it.  Got that?
	 */
	freespace = INSERT_FREESPACE(Insert);
	if (freespace < SizeOfXLogRecord)
	{
		(void) AdvanceXLInsertBuffer();
		/* OK to ignore update return flag, since we will do flush anyway */
		freespace = BLCKSZ - SizeOfXLogPHD;
	}
	INSERT_RECPTR(checkPoint.redo, Insert, Insert->curridx);
	/*
	 * Here we update the shared RedoRecPtr for future XLogInsert calls;
	 * this must be done while holding the insert lock.
	 */
	RedoRecPtr = XLogCtl->Insert.RedoRecPtr = checkPoint.redo;
	/*
	 * Get UNDO record ptr - this is oldest of PROC->logRec values.
	 * We do this while holding insert lock to ensure that we won't miss
	 * any about-to-commit transactions (UNDO must include all xacts that
	 * have commits after REDO point).
	 */
	checkPoint.undo = GetUndoRecPtr();

	if (shutdown && checkPoint.undo.xrecoff != 0)
		elog(STOP, "Active transaction while data base is shutting down");

	/*
	 * Now we can release insert lock, allowing other xacts to proceed
	 * even while we are flushing disk buffers.
	 */
	S_UNLOCK(&(XLogCtl->insert_lck));

	SpinAcquire(XidGenLockId);
	checkPoint.nextXid = ShmemVariableCache->nextXid;
	if (!shutdown)
		checkPoint.nextXid += ShmemVariableCache->xidCount;
	SpinRelease(XidGenLockId);

	SpinAcquire(OidGenLockId);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;
	SpinRelease(OidGenLockId);

	/*
	 * Having constructed the checkpoint record, ensure all shmem disk buffers
	 * are flushed to disk.
	 */
	FlushBufferPool();

	/*
	 * Now insert the checkpoint record into XLOG.
	 */
	rdata.buffer = InvalidBuffer;
	rdata.data = (char *)(&checkPoint);
	rdata.len = sizeof(checkPoint);
	rdata.next = NULL;

	recptr = XLogInsert(RM_XLOG_ID,
						shutdown ? XLOG_CHECKPOINT_SHUTDOWN :
						XLOG_CHECKPOINT_ONLINE,
						&rdata);

	XLogFlush(recptr);

	/*
	 * We now have ProcLastRecPtr = start of actual checkpoint record,
	 * recptr = end of actual checkpoint record.
	 */
	if (shutdown && !XLByteEQ(checkPoint.redo, ProcLastRecPtr))
		elog(STOP, "XLog concurrent activity while data base is shutting down");

	/*
	 * Remember location of prior checkpoint's earliest info.
	 * Oldest item is redo or undo, whichever is older; but watch out
	 * for case that undo = 0.
	 */
	if (ControlFile->checkPointCopy.undo.xrecoff != 0 && 
		XLByteLT(ControlFile->checkPointCopy.undo,
				 ControlFile->checkPointCopy.redo))
		XLByteToSeg(ControlFile->checkPointCopy.undo, _logId, _logSeg);
	else
		XLByteToSeg(ControlFile->checkPointCopy.redo, _logId, _logSeg);

	/*
	 * Update the control file.
	 */
	SpinAcquire(ControlFileLockId);
	if (shutdown)
		ControlFile->state = DB_SHUTDOWNED;
	ControlFile->prevCheckPoint = ControlFile->checkPoint;
	ControlFile->checkPoint = ProcLastRecPtr;
	ControlFile->checkPointCopy = checkPoint;
	ControlFile->time = time(NULL);
	UpdateControlFile();
	SpinRelease(ControlFileLockId);

	/*
	 * Delete offline log files (those no longer needed even for previous
	 * checkpoint).
	 */
	if (_logId || _logSeg)
	{
		PrevLogSeg(_logId, _logSeg);
		MoveOfflineLogs(_logId, _logSeg);
	}

	/*
	 * Make more log segments if needed.  (Do this after deleting offline
	 * log segments, to avoid having peak disk space usage higher than
	 * necessary.)
	 */
	if (!shutdown)
		PreallocXlogFiles(recptr);

	S_UNLOCK(&(XLogCtl->chkp_lck));

	END_CRIT_SECTION();
}

/*
 * Write a NEXTXID log record
 */
void
XLogPutNextXid(TransactionId nextXid)
{
	XLogRecData		rdata;

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *)(&nextXid);
	rdata.len = sizeof(TransactionId);
	rdata.next = NULL;
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTXID, &rdata);
}

/*
 * Write a NEXTOID log record
 */
void
XLogPutNextOid(Oid nextOid)
{
	XLogRecData		rdata;

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *)(&nextOid);
	rdata.len = sizeof(Oid);
	rdata.next = NULL;
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTOID, &rdata);
}

/*
 * XLOG resource manager's routines
 */
void
xlog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_NEXTXID)
	{
		TransactionId		nextXid;

		memcpy(&nextXid, XLogRecGetData(record), sizeof(TransactionId));
		if (ShmemVariableCache->nextXid < nextXid)
		{
			ShmemVariableCache->nextXid = nextXid;
			ShmemVariableCache->xidCount = 0;
		}
	}
	else if (info == XLOG_NEXTOID)
	{
		Oid		nextOid;

		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		if (ShmemVariableCache->nextOid < nextOid)
		{
			ShmemVariableCache->nextOid = nextOid;
			ShmemVariableCache->oidCount = 0;
		}
	}
	else if (info == XLOG_CHECKPOINT_SHUTDOWN)
	{
		CheckPoint	checkPoint;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In a SHUTDOWN checkpoint, believe the counters exactly */
		ShmemVariableCache->nextXid = checkPoint.nextXid;
		ShmemVariableCache->xidCount = 0;
		ShmemVariableCache->nextOid = checkPoint.nextOid;
		ShmemVariableCache->oidCount = 0;
	}
	else if (info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	checkPoint;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In an ONLINE checkpoint, treat the counters like NEXTXID/NEXTOID */
		if (ShmemVariableCache->nextXid < checkPoint.nextXid)
		{
			ShmemVariableCache->nextXid = checkPoint.nextXid;
			ShmemVariableCache->xidCount = 0;
		}
		if (ShmemVariableCache->nextOid < checkPoint.nextOid)
		{
			ShmemVariableCache->nextOid = checkPoint.nextOid;
			ShmemVariableCache->oidCount = 0;
		}
	}
}
 
void
xlog_undo(XLogRecPtr lsn, XLogRecord *record)
{
}
 
void
xlog_desc(char *buf, uint8 xl_info, char* rec)
{
	uint8	info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_CHECKPOINT_SHUTDOWN ||
		info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	*checkpoint = (CheckPoint*) rec;
		sprintf(buf + strlen(buf), "checkpoint: redo %u/%u; undo %u/%u; "
		"sui %u; xid %u; oid %u; %s",
			checkpoint->redo.xlogid, checkpoint->redo.xrecoff,
			checkpoint->undo.xlogid, checkpoint->undo.xrecoff,
			checkpoint->ThisStartUpID, checkpoint->nextXid, 
			checkpoint->nextOid,
			(info == XLOG_CHECKPOINT_SHUTDOWN) ? "shutdown" : "online");
	}
	else if (info == XLOG_NEXTXID)
	{
		TransactionId		nextXid;

		memcpy(&nextXid, rec, sizeof(TransactionId));
		sprintf(buf + strlen(buf), "nextXid: %u", nextXid);
	}
	else if (info == XLOG_NEXTOID)
	{
		Oid		nextOid;

		memcpy(&nextOid, rec, sizeof(Oid));
		sprintf(buf + strlen(buf), "nextOid: %u", nextOid);
	}
	else
		strcat(buf, "UNKNOWN");
}

static void
xlog_outrec(char *buf, XLogRecord *record)
{
	int		bkpb;
	int		i;

	sprintf(buf + strlen(buf), "prev %u/%u; xprev %u/%u; xid %u",
		record->xl_prev.xlogid, record->xl_prev.xrecoff,
		record->xl_xact_prev.xlogid, record->xl_xact_prev.xrecoff,
		record->xl_xid);

	for (i = 0, bkpb = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & (XLR_SET_BKP_BLOCK(i))))
			continue;
		bkpb++;
	}

	if (bkpb)
		sprintf(buf + strlen(buf), "; bkpb %d", bkpb);

	sprintf(buf + strlen(buf), ": %s",
		RmgrTable[record->xl_rmid].rm_name);
}


/*
 * GUC support routines
 */

bool
check_xlog_sync_method(const char *method)
{
	if (strcasecmp(method, "fsync") == 0) return true;
#ifdef HAVE_FDATASYNC
	if (strcasecmp(method, "fdatasync") == 0) return true;
#endif
#ifdef OPEN_SYNC_FLAG
	if (strcasecmp(method, "open_sync") == 0) return true;
#endif
#ifdef OPEN_DATASYNC_FLAG
	if (strcasecmp(method, "open_datasync") == 0) return true;
#endif
	return false;
}

void
assign_xlog_sync_method(const char *method)
{
	int		new_sync_method;
	int		new_sync_bit;

	if (strcasecmp(method, "fsync") == 0)
	{
		new_sync_method = SYNC_METHOD_FSYNC;
		new_sync_bit = 0;
	}
#ifdef HAVE_FDATASYNC
	else if (strcasecmp(method, "fdatasync") == 0)
	{
		new_sync_method = SYNC_METHOD_FDATASYNC;
		new_sync_bit = 0;
	}
#endif
#ifdef OPEN_SYNC_FLAG
	else if (strcasecmp(method, "open_sync") == 0)
	{
		new_sync_method = SYNC_METHOD_OPEN;
		new_sync_bit = OPEN_SYNC_FLAG;
	}
#endif
#ifdef OPEN_DATASYNC_FLAG
	else if (strcasecmp(method, "open_datasync") == 0)
	{
		new_sync_method = SYNC_METHOD_OPEN;
		new_sync_bit = OPEN_DATASYNC_FLAG;
	}
#endif
	else
	{
		/* Can't get here unless guc.c screwed up */
		elog(ERROR, "Bogus xlog sync method %s", method);
		new_sync_method = 0;	/* keep compiler quiet */
		new_sync_bit = 0;
	}

	if (sync_method != new_sync_method || open_sync_bit != new_sync_bit)
	{
		/*
		 * To ensure that no blocks escape unsynced, force an fsync on
		 * the currently open log segment (if any).  Also, if the open
		 * flag is changing, close the log file so it will be reopened
		 * (with new flag bit) at next use.
		 */
		if (openLogFile >= 0)
		{
			if (pg_fsync(openLogFile) != 0)
				elog(STOP, "fsync(logfile %u seg %u) failed: %m",
					 openLogId, openLogSeg);
			if (open_sync_bit != new_sync_bit)
			{
				if (close(openLogFile) != 0)
					elog(STOP, "close(logfile %u seg %u) failed: %m",
						 openLogId, openLogSeg);
				openLogFile = -1;
			}
		}
		sync_method = new_sync_method;
		open_sync_bit = new_sync_bit;
	}
}


/*
 * Issue appropriate kind of fsync (if any) on the current XLOG output file
 */
static void
issue_xlog_fsync(void)
{
	switch (sync_method)
	{
		case SYNC_METHOD_FSYNC:
			if (pg_fsync(openLogFile) != 0)
				elog(STOP, "fsync(logfile %u seg %u) failed: %m",
					 openLogId, openLogSeg);
			break;
#ifdef HAVE_FDATASYNC
		case SYNC_METHOD_FDATASYNC:
			if (pg_fdatasync(openLogFile) != 0)
				elog(STOP, "fdatasync(logfile %u seg %u) failed: %m",
					 openLogId, openLogSeg);
			break;
#endif
		case SYNC_METHOD_OPEN:
			/* write synced it already */
			break;
		default:
			elog(STOP, "bogus sync_method %d", sync_method);
			break;
	}
}
