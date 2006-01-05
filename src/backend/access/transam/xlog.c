/*-------------------------------------------------------------------------
 *
 * xlog.c
 *		PostgreSQL transaction log manager
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/access/transam/xlog.c,v 1.125.2.4 2006/01/05 00:55:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "access/clog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "catalog/catversion.h"
#include "catalog/pg_control.h"
#include "storage/bufpage.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/relcache.h"
#include "miscadmin.h"


/*
 * This chunk of hackery attempts to determine which file sync methods
 * are available on the current platform, and to choose an appropriate
 * default method.	We assume that fsync() is always available, and that
 * configure determined whether fdatasync() is.
 */
#define SYNC_METHOD_FSYNC		0
#define SYNC_METHOD_FDATASYNC	1
#define SYNC_METHOD_OPEN		2		/* used for both O_SYNC and
										 * O_DSYNC */

#if defined(O_SYNC)
#define OPEN_SYNC_FLAG	   O_SYNC
#else
#if defined(O_FSYNC)
#define OPEN_SYNC_FLAG	  O_FSYNC
#endif
#endif

#if defined(OPEN_SYNC_FLAG)
#if defined(O_DSYNC) && (O_DSYNC != OPEN_SYNC_FLAG)
#define OPEN_DATASYNC_FLAG	  O_DSYNC
#endif
#endif

#if defined(OPEN_DATASYNC_FLAG)
#define DEFAULT_SYNC_METHOD_STR    "open_datasync"
#define DEFAULT_SYNC_METHOD		   SYNC_METHOD_OPEN
#define DEFAULT_SYNC_FLAGBIT	   OPEN_DATASYNC_FLAG
#else
#if defined(HAVE_FDATASYNC)
#define DEFAULT_SYNC_METHOD_STR   "fdatasync"
#define DEFAULT_SYNC_METHOD		  SYNC_METHOD_FDATASYNC
#define DEFAULT_SYNC_FLAGBIT	  0
#else
#define DEFAULT_SYNC_METHOD_STR   "fsync"
#define DEFAULT_SYNC_METHOD		  SYNC_METHOD_FSYNC
#define DEFAULT_SYNC_FLAGBIT	  0
#endif
#endif


/* User-settable parameters */
int			CheckPointSegments = 3;
int			XLOGbuffers = 8;
int			XLOG_DEBUG = 0;
char	   *XLOG_sync_method = NULL;
const char	XLOG_sync_method_default[] = DEFAULT_SYNC_METHOD_STR;
char		XLOG_archive_dir[MAXPGPATH];		/* null string means
												 * delete 'em */

/*
 * XLOGfileslop is used in the code as the allowed "fuzz" in the number of
 * preallocated XLOG segments --- we try to have at least XLOGfiles advance
 * segments but no more than XLOGfileslop segments.  This could
 * be made a separate GUC variable, but at present I think it's sufficient
 * to hardwire it as 2*CheckPointSegments+1.  Under normal conditions, a
 * checkpoint will free no more than 2*CheckPointSegments log segments, and
 * we want to recycle all of them; the +1 allows boundary cases to happen
 * without wasting a delete/create-segment cycle.
 */

#define XLOGfileslop	(2*CheckPointSegments + 1)


/* these are derived from XLOG_sync_method by assign_xlog_sync_method */
static int	sync_method = DEFAULT_SYNC_METHOD;
static int	open_sync_bit = DEFAULT_SYNC_FLAGBIT;

#define XLOG_SYNC_BIT  (enableFsync ? open_sync_bit : 0)

#define MinXLOGbuffers	4


/*
 * ThisStartUpID will be same in all backends --- it identifies current
 * instance of the database system.
 */
StartUpID	ThisStartUpID = 0;

/* Are we doing recovery by reading XLOG? */
bool		InRecovery = false;

/*
 * MyLastRecPtr points to the start of the last XLOG record inserted by the
 * current transaction.  If MyLastRecPtr.xrecoff == 0, then the current
 * xact hasn't yet inserted any transaction-controlled XLOG records.
 *
 * Note that XLOG records inserted outside transaction control are not
 * reflected into MyLastRecPtr.  They do, however, cause MyXactMadeXLogEntry
 * to be set true.	The latter can be used to test whether the current xact
 * made any loggable changes (including out-of-xact changes, such as
 * sequence updates).
 *
 * When we insert/update/delete a tuple in a temporary relation, we do not
 * make any XLOG record, since we don't care about recovering the state of
 * the temp rel after a crash.	However, we will still need to remember
 * whether our transaction committed or aborted in that case.  So, we must
 * set MyXactMadeTempRelUpdate true to indicate that the XID will be of
 * interest later.
 */
XLogRecPtr	MyLastRecPtr = {0, 0};

bool		MyXactMadeXLogEntry = false;

bool		MyXactMadeTempRelUpdate = false;

/*
 * ProcLastRecPtr points to the start of the last XLOG record inserted by the
 * current backend.  It is updated for all inserts, transaction-controlled
 * or not.	ProcLastRecEnd is similar but points to end+1 of last record.
 */
static XLogRecPtr ProcLastRecPtr = {0, 0};

XLogRecPtr	ProcLastRecEnd = {0, 0};

/*
 * RedoRecPtr is this backend's local copy of the REDO record pointer
 * (which is almost but not quite the same as a pointer to the most recent
 * CHECKPOINT record).	We update this from the shared-memory copy,
 * XLogCtl->Insert.RedoRecPtr, whenever we can safely do so (ie, when we
 * hold the Insert lock).  See XLogInsert for details.	We are also allowed
 * to update from XLogCtl->Insert.RedoRecPtr if we hold the info_lck;
 * see GetRedoRecPtr.
 */
static XLogRecPtr RedoRecPtr;

/*----------
 * Shared-memory data structures for XLOG control
 *
 * LogwrtRqst indicates a byte position that we need to write and/or fsync
 * the log up to (all records before that point must be written or fsynced).
 * LogwrtResult indicates the byte positions we have already written/fsynced.
 * These structs are identical but are declared separately to indicate their
 * slightly different functions.
 *
 * We do a lot of pushups to minimize the amount of access to lockable
 * shared memory values.  There are actually three shared-memory copies of
 * LogwrtResult, plus one unshared copy in each backend.  Here's how it works:
 *		XLogCtl->LogwrtResult is protected by info_lck
 *		XLogCtl->Write.LogwrtResult is protected by WALWriteLock
 *		XLogCtl->Insert.LogwrtResult is protected by WALInsertLock
 * One must hold the associated lock to read or write any of these, but
 * of course no lock is needed to read/write the unshared LogwrtResult.
 *
 * XLogCtl->LogwrtResult and XLogCtl->Write.LogwrtResult are both "always
 * right", since both are updated by a write or flush operation before
 * it releases WALWriteLock.  The point of keeping XLogCtl->Write.LogwrtResult
 * is that it can be examined/modified by code that already holds WALWriteLock
 * without needing to grab info_lck as well.
 *
 * XLogCtl->Insert.LogwrtResult may lag behind the reality of the other two,
 * but is updated when convenient.	Again, it exists for the convenience of
 * code that is already holding WALInsertLock but not the other locks.
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
 *
 * info_lck is only held long enough to read/update the protected variables,
 * so it's a plain spinlock.  The other locks are held longer (potentially
 * over I/O operations), so we use LWLocks for them.  These locks are:
 *
 * WALInsertLock: must be held to insert a record into the WAL buffers.
 *
 * WALWriteLock: must be held to write WAL buffers to disk (XLogWrite or
 * XLogFlush).
 *
 * ControlFileLock: must be held to read/update control file or create
 * new log file.
 *
 * CheckpointLock: must be held to do a checkpoint (ensures only one
 * checkpointer at a time; even though the postmaster won't launch
 * parallel checkpoint processes, we need this because manual checkpoints
 * could be launched simultaneously).
 *
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
	XLogwrtResult LogwrtResult; /* a recent value of LogwrtResult */
	XLogRecPtr	PrevRecord;		/* start of previously-inserted record */
	uint16		curridx;		/* current block index in cache */
	XLogPageHeader currpage;	/* points to header of block in cache */
	char	   *currpos;		/* current insertion point in cache */
	XLogRecPtr	RedoRecPtr;		/* current redo point for insertions */
} XLogCtlInsert;

/*
 * Shared state data for XLogWrite/XLogFlush.
 */
typedef struct XLogCtlWrite
{
	XLogwrtResult LogwrtResult; /* current value of LogwrtResult */
	uint16		curridx;		/* cache index of next block to write */
} XLogCtlWrite;

/*
 * Total shared-memory state for XLOG.
 */
typedef struct XLogCtlData
{
	/* Protected by WALInsertLock: */
	XLogCtlInsert Insert;
	/* Protected by info_lck: */
	XLogwrtRqst LogwrtRqst;
	XLogwrtResult LogwrtResult;
	/* Protected by WALWriteLock: */
	XLogCtlWrite Write;

	/*
	 * These values do not change after startup, although the pointed-to
	 * pages and xlblocks values certainly do.	Permission to read/write
	 * the pages and xlblocks values depends on WALInsertLock and
	 * WALWriteLock.
	 */
	char	   *pages;			/* buffers for unwritten XLOG pages */
	XLogRecPtr *xlblocks;		/* 1st byte ptr-s + BLCKSZ */
	uint32		XLogCacheByte;	/* # bytes in xlog buffers */
	uint32		XLogCacheBlck;	/* highest allocated xlog buffer index */
	StartUpID	ThisStartUpID;

	/* This value is not protected by *any* lock... */
	/* see SetSavedRedoRecPtr/GetSavedRedoRecPtr */
	XLogRecPtr	SavedRedoRecPtr;

	slock_t		info_lck;		/* locks shared LogwrtRqst/LogwrtResult */
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
 * a boundary byte is taken to be in the previous segment.	This is suitable
 * for deciding which segment to write given a pointer to a record end,
 * for example.  (We can assume xrecoff is not zero, since no valid recptr
 * can have that.)
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
			snprintf(path, MAXPGPATH, "%s/%08X%08X",	\
					 XLogDir, log, seg)

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
static char XLogDir[MAXPGPATH];
static char ControlFilePath[MAXPGPATH];

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
static XLogRecPtr ReadRecPtr;				/* start of last record read */
static XLogRecPtr EndRecPtr;				/* end+1 of last record read */
static XLogRecord *nextRecord = NULL;
static StartUpID lastReadSUI;

static bool InRedo = false;


static bool AdvanceXLInsertBuffer(void);
static void XLogWrite(XLogwrtRqst WriteRqst);
static int XLogFileInit(uint32 log, uint32 seg,
			 bool *use_existent, bool use_lock);
static bool InstallXLogFileSegment(uint32 log, uint32 seg, char *tmppath,
					   bool find_free, int max_advance,
					   bool use_lock);
static int	XLogFileOpen(uint32 log, uint32 seg, bool econt);
static void PreallocXlogFiles(XLogRecPtr endptr);
static void MoveOfflineLogs(uint32 log, uint32 seg, XLogRecPtr endptr);
static XLogRecord *ReadRecord(XLogRecPtr *RecPtr, int emode, char *buffer);
static bool ValidXLOGHeader(XLogPageHeader hdr, int emode, bool checkSUI);
static XLogRecord *ReadCheckpointRecord(XLogRecPtr RecPtr,
					 int whichChkpt,
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
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogRecord *record;
	XLogContRecord *contrecord;
	XLogRecPtr	RecPtr;
	XLogRecPtr	WriteRqst;
	uint32		freespace;
	uint16		curridx;
	XLogRecData *rdt;
	Buffer		dtbuf[XLR_MAX_BKP_BLOCKS];
	bool		dtbuf_bkp[XLR_MAX_BKP_BLOCKS];
	BkpBlock	dtbuf_xlg[XLR_MAX_BKP_BLOCKS];
	XLogRecPtr	dtbuf_lsn[XLR_MAX_BKP_BLOCKS];
	XLogRecData dtbuf_rdt[2 * XLR_MAX_BKP_BLOCKS];
	crc64		rdata_crc;
	uint32		len,
				write_len;
	unsigned	i;
	XLogwrtRqst LogwrtRqst;
	bool		updrqst;
	bool		no_tran = (rmid == RM_XLOG_ID) ? true : false;

	if (info & XLR_INFO_MASK)
	{
		if ((info & XLR_INFO_MASK) != XLOG_NO_TRAN)
			elog(PANIC, "invalid xlog info mask %02X", (info & XLR_INFO_MASK));
		no_tran = true;
		info &= ~XLR_INFO_MASK;
	}

	/*
	 * In bootstrap mode, we don't actually log anything but XLOG
	 * resources; return a phony record pointer.
	 */
	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		RecPtr.xlogid = 0;
		RecPtr.xrecoff = SizeOfXLogPHD; /* start of 1st checkpoint record */
		return (RecPtr);
	}

	/*
	 * Here we scan the rdata list, determine which buffers must be backed
	 * up, and compute the CRC values for the data.  Note that the record
	 * header isn't added into the CRC yet since we don't know the final
	 * length or info bits quite yet.
	 *
	 * We may have to loop back to here if a race condition is detected
	 * below. We could prevent the race by doing all this work while
	 * holding the insert lock, but it seems better to avoid doing CRC
	 * calculations while holding the lock.  This means we have to be
	 * careful about modifying the rdata list until we know we aren't
	 * going to loop back again.  The only change we allow ourselves to
	 * make earlier is to set rdt->data = NULL in list items we have
	 * decided we will have to back up the whole buffer for.  This is OK
	 * because we will certainly decide the same thing again for those
	 * items if we do it over; doing it here saves an extra pass over the
	 * list later.
	 */
begin:;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		dtbuf[i] = InvalidBuffer;
		dtbuf_bkp[i] = false;
	}

	INIT_CRC64(rdata_crc);
	len = 0;
	for (rdt = rdata;;)
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
					dtbuf_lsn[i] = *((XLogRecPtr *) BufferGetBlock(rdt->buffer));
					if (XLByteLE(dtbuf_lsn[i], RedoRecPtr))
					{
						crc64		dtcrc;

						dtbuf_bkp[i] = true;
						rdt->data = NULL;
						INIT_CRC64(dtcrc);
						COMP_CRC64(dtcrc,
								   BufferGetBlock(dtbuf[i]),
								   BLCKSZ);
						dtbuf_xlg[i].node = BufferGetFileNode(dtbuf[i]);
						dtbuf_xlg[i].block = BufferGetBlockNumber(dtbuf[i]);
						COMP_CRC64(dtcrc,
								(char *) &(dtbuf_xlg[i]) + sizeof(crc64),
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
				elog(PANIC, "can backup at most %d blocks per xlog record",
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
	 * blocks.	Currently, all callers of XLogInsert provide at least some
	 * not-in-a-buffer data and so len == 0 should never happen, but that
	 * may not be true forever.  If you need to remove the len == 0 check,
	 * also remove the check for xl_len == 0 in ReadRecord, below.
	 */
	if (len == 0 || len > MAXLOGRECSZ)
		elog(PANIC, "invalid xlog record length %u", len);

	START_CRIT_SECTION();

	/* update LogwrtResult before doing cache fill check */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile XLogCtlData *xlogctl = XLogCtl;

		SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
		LogwrtRqst = xlogctl->LogwrtRqst;
		LogwrtResult = xlogctl->LogwrtResult;
		SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
	}

	/*
	 * If cache is half filled then try to acquire write lock and do
	 * XLogWrite. Ignore any fractional blocks in performing this check.
	 */
	LogwrtRqst.Write.xrecoff -= LogwrtRqst.Write.xrecoff % BLCKSZ;
	if (LogwrtRqst.Write.xlogid != LogwrtResult.Write.xlogid ||
		(LogwrtRqst.Write.xrecoff >= LogwrtResult.Write.xrecoff +
		 XLogCtl->XLogCacheByte / 2))
	{
		if (LWLockConditionalAcquire(WALWriteLock, LW_EXCLUSIVE))
		{
			LogwrtResult = XLogCtl->Write.LogwrtResult;
			if (XLByteLT(LogwrtResult.Write, LogwrtRqst.Write))
				XLogWrite(LogwrtRqst);
			LWLockRelease(WALWriteLock);
		}
	}

	/* Now wait to get insert lock */
	LWLockAcquire(WALInsertLock, LW_EXCLUSIVE);

	/*
	 * Check to see if my RedoRecPtr is out of date.  If so, may have to
	 * go back and recompute everything.  This can only happen just after
	 * a checkpoint, so it's better to be slow in this case and fast
	 * otherwise.
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
				 * Oops, this buffer now needs to be backed up, but we
				 * didn't think so above.  Start over.
				 */
				LWLockRelease(WALInsertLock);
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
	 * up.	The i'th XLR_SET_BKP_BLOCK bit corresponds to the i'th
	 * distinct buffer value (ignoring InvalidBuffer) appearing in the
	 * rdata list.
	 */
	write_len = len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (dtbuf[i] == InvalidBuffer || !(dtbuf_bkp[i]))
			continue;

		info |= XLR_SET_BKP_BLOCK(i);

		rdt->next = &(dtbuf_rdt[2 * i]);

		dtbuf_rdt[2 * i].data = (char *) &(dtbuf_xlg[i]);
		dtbuf_rdt[2 * i].len = sizeof(BkpBlock);
		write_len += sizeof(BkpBlock);

		rdt = dtbuf_rdt[2 * i].next = &(dtbuf_rdt[2 * i + 1]);

		dtbuf_rdt[2 * i + 1].data = (char *) BufferGetBlock(dtbuf[i]);
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
	COMP_CRC64(rdata_crc, (char *) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(rdata_crc);
	record->xl_crc = rdata_crc;

	/* Compute record's XLOG location */
	INSERT_RECPTR(RecPtr, Insert, curridx);

	/* If first XLOG record of transaction, save it in PGPROC array */
	if (MyLastRecPtr.xrecoff == 0 && !no_tran)
	{
		/*
		 * We do not acquire SInvalLock here because of possible deadlock.
		 * Anyone who wants to inspect other procs' logRec must acquire
		 * WALInsertLock, instead.	A better solution would be a per-PROC
		 * spinlock, but no time for that before 7.2 --- tgl 12/19/01.
		 */
		MyProc->logRec = RecPtr;
	}

	if (XLOG_DEBUG)
	{
		char		buf[8192];

		sprintf(buf, "INSERT @ %X/%X: ", RecPtr.xlogid, RecPtr.xrecoff);
		xlog_outrec(buf, record);
		if (rdata->data != NULL)
		{
			strcat(buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(buf, record->xl_info, rdata->data);
		}
		elog(LOG, "%s", buf);
	}

	/* Record begin of record in appropriate places */
	if (!no_tran)
		MyLastRecPtr = RecPtr;
	ProcLastRecPtr = RecPtr;
	Insert->PrevRecord = RecPtr;
	MyXactMadeXLogEntry = true;

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
	 * The recptr I return is the beginning of the *next* record. This
	 * will be stored as LSN for changed data pages...
	 */
	INSERT_RECPTR(RecPtr, Insert, curridx);

	/* Need to update shared LogwrtRqst if some block was filled up */
	if (freespace < SizeOfXLogRecord)
		updrqst = true;			/* curridx is filled and available for
								 * writing out */
	else
		curridx = PrevBufIdx(curridx);
	WriteRqst = XLogCtl->xlblocks[curridx];

	LWLockRelease(WALInsertLock);

	if (updrqst)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile XLogCtlData *xlogctl = XLogCtl;

		SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
		/* advance global request to include new block(s) */
		if (XLByteLT(xlogctl->LogwrtRqst.Write, WriteRqst))
			xlogctl->LogwrtRqst.Write = WriteRqst;
		/* update local result copy while I have the chance */
		LogwrtResult = xlogctl->LogwrtResult;
		SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
	}

	ProcLastRecEnd = RecPtr;

	END_CRIT_SECTION();

	return (RecPtr);
}

/*
 * Advance the Insert state to the next buffer page, writing out the next
 * buffer if it still contains unwritten data.
 *
 * The global LogwrtRqst.Write pointer needs to be advanced to include the
 * just-filled page.  If we can do this for free (without an extra lock),
 * we do so here.  Otherwise the caller must do it.  We return TRUE if the
 * request update still needs to be done, FALSE if we did it internally.
 *
 * Must be called with WALInsertLock held.
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
	XLogRecPtr	NewPageEndPtr;
	XLogPageHeader NewPage;

	/* Use Insert->LogwrtResult copy if it's more fresh */
	if (XLByteLT(LogwrtResult.Write, Insert->LogwrtResult.Write))
		LogwrtResult = Insert->LogwrtResult;

	/*
	 * Get ending-offset of the buffer page we need to replace (this may
	 * be zero if the buffer hasn't been used yet).  Fall through if it's
	 * already written out.
	 */
	OldPageRqstPtr = XLogCtl->xlblocks[nextidx];
	if (!XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
	{
		/* nope, got work to do... */
		XLogRecPtr	FinishedPageRqstPtr;

		FinishedPageRqstPtr = XLogCtl->xlblocks[Insert->curridx];

		/* Before waiting, get info_lck and update LogwrtResult */
		{
			/* use volatile pointer to prevent code rearrangement */
			volatile XLogCtlData *xlogctl = XLogCtl;

			SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
			if (XLByteLT(xlogctl->LogwrtRqst.Write, FinishedPageRqstPtr))
				xlogctl->LogwrtRqst.Write = FinishedPageRqstPtr;
			LogwrtResult = xlogctl->LogwrtResult;
			SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
		}

		update_needed = false;	/* Did the shared-request update */

		if (XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
		{
			/* OK, someone wrote it already */
			Insert->LogwrtResult = LogwrtResult;
		}
		else
		{
			/* Must acquire write lock */
			LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
			LogwrtResult = Write->LogwrtResult;
			if (XLByteLE(OldPageRqstPtr, LogwrtResult.Write))
			{
				/* OK, someone wrote it already */
				LWLockRelease(WALWriteLock);
				Insert->LogwrtResult = LogwrtResult;
			}
			else
			{
				/*
				 * Have to write buffers while holding insert lock. This
				 * is not good, so only write as much as we absolutely
				 * must.
				 */
				WriteRqst.Write = OldPageRqstPtr;
				WriteRqst.Flush.xlogid = 0;
				WriteRqst.Flush.xrecoff = 0;
				XLogWrite(WriteRqst);
				LWLockRelease(WALWriteLock);
				Insert->LogwrtResult = LogwrtResult;
			}
		}
	}

	/*
	 * Now the next buffer slot is free and we can set it up to be the
	 * next output page.
	 */
	NewPageEndPtr = XLogCtl->xlblocks[Insert->curridx];
	if (NewPageEndPtr.xrecoff >= XLogFileSize)
	{
		/* crossing a logid boundary */
		NewPageEndPtr.xlogid += 1;
		NewPageEndPtr.xrecoff = BLCKSZ;
	}
	else
		NewPageEndPtr.xrecoff += BLCKSZ;
	XLogCtl->xlblocks[nextidx] = NewPageEndPtr;
	NewPage = (XLogPageHeader) (XLogCtl->pages + nextidx * BLCKSZ);
	Insert->curridx = nextidx;
	Insert->currpage = NewPage;
	Insert->currpos = ((char *) NewPage) + SizeOfXLogPHD;

	/*
	 * Be sure to re-zero the buffer so that bytes beyond what we've
	 * written will look like zeroes and not valid XLOG records...
	 */
	MemSet((char *) NewPage, 0, BLCKSZ);

	/* And fill the new page's header */
	NewPage->xlp_magic = XLOG_PAGE_MAGIC;
	/* NewPage->xlp_info = 0; */	/* done by memset */
	NewPage->xlp_sui = ThisStartUpID;
	NewPage->xlp_pageaddr.xlogid = NewPageEndPtr.xlogid;
	NewPage->xlp_pageaddr.xrecoff = NewPageEndPtr.xrecoff - BLCKSZ;

	return update_needed;
}

/*
 * Write and/or fsync the log at least as far as WriteRqst indicates.
 *
 * Must be called with WALWriteLock held.
 */
static void
XLogWrite(XLogwrtRqst WriteRqst)
{
	XLogCtlWrite *Write = &XLogCtl->Write;
	char	   *from;
	bool		ispartialpage;
	bool		use_existent;

	/*
	 * Update local LogwrtResult (caller probably did this already,
	 * but...)
	 */
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
			elog(PANIC, "xlog write request %X/%X is past end of log %X/%X",
				 LogwrtResult.Write.xlogid, LogwrtResult.Write.xrecoff,
				 XLogCtl->xlblocks[Write->curridx].xlogid,
				 XLogCtl->xlblocks[Write->curridx].xrecoff);

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
					ereport(PANIC,
							(errcode_for_file_access(),
					errmsg("could not close log file %u, segment %u: %m",
						   openLogId, openLogSeg)));
				openLogFile = -1;
			}
			XLByteToPrevSeg(LogwrtResult.Write, openLogId, openLogSeg);

			/* create/use new log file */
			use_existent = true;
			openLogFile = XLogFileInit(openLogId, openLogSeg,
									   &use_existent, true);
			openLogOff = 0;

			/* update pg_control, unless someone else already did */
			LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
			if (ControlFile->logId < openLogId ||
				(ControlFile->logId == openLogId &&
				 ControlFile->logSeg < openLogSeg + 1))
			{
				ControlFile->logId = openLogId;
				ControlFile->logSeg = openLogSeg + 1;
				ControlFile->time = time(NULL);
				UpdateControlFile();

				/*
				 * Signal postmaster to start a checkpoint if it's been
				 * too long since the last one.  (We look at local copy of
				 * RedoRecPtr which might be a little out of date, but
				 * should be close enough for this purpose.)
				 */
				if (IsUnderPostmaster &&
					(openLogId != RedoRecPtr.xlogid ||
					 openLogSeg >= (RedoRecPtr.xrecoff / XLogSegSize) +
					 (uint32) CheckPointSegments))
				{
					if (XLOG_DEBUG)
						elog(LOG, "time for a checkpoint, signaling postmaster");
					SendPostmasterSignal(PMSIGNAL_DO_CHECKPOINT);
				}
			}
			LWLockRelease(ControlFileLock);
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
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not seek in log file %u, segment %u to offset %u: %m",
								openLogId, openLogSeg, openLogOff)));
		}

		/* OK to write the page */
		from = XLogCtl->pages + Write->curridx * BLCKSZ;
		errno = 0;
		if (write(openLogFile, from, BLCKSZ) != BLCKSZ)
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to log file %u, segment %u at offset %u: %m",
							openLogId, openLogSeg, openLogOff)));
		}
		openLogOff += BLCKSZ;

		/*
		 * If we just wrote the whole last page of a logfile segment,
		 * fsync the segment immediately.  This avoids having to go back
		 * and re-open prior segments when an fsync request comes along
		 * later. Doing it here ensures that one and only one backend will
		 * perform this fsync.
		 */
		if (openLogOff >= XLogSegSize && !ispartialpage)
		{
			issue_xlog_fsync();
			LogwrtResult.Flush = LogwrtResult.Write;	/* end of current page */
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
		 * Could get here without iterating above loop, in which case we
		 * might have no open file or the wrong one.  However, we do not
		 * need to fsync more than one file.
		 */
		if (sync_method != SYNC_METHOD_OPEN)
		{
			if (openLogFile >= 0 &&
			 !XLByteInPrevSeg(LogwrtResult.Write, openLogId, openLogSeg))
			{
				if (close(openLogFile) != 0)
					ereport(PANIC,
							(errcode_for_file_access(),
					errmsg("could not close log file %u, segment %u: %m",
						   openLogId, openLogSeg)));
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
	 * We make sure that the shared 'request' values do not fall behind the
	 * 'result' values.  This is not absolutely essential, but it saves
	 * some code in a couple of places.
	 */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile XLogCtlData *xlogctl = XLogCtl;

		SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
		xlogctl->LogwrtResult = LogwrtResult;
		if (XLByteLT(xlogctl->LogwrtRqst.Write, LogwrtResult.Write))
			xlogctl->LogwrtRqst.Write = LogwrtResult.Write;
		if (XLByteLT(xlogctl->LogwrtRqst.Flush, LogwrtResult.Flush))
			xlogctl->LogwrtRqst.Flush = LogwrtResult.Flush;
		SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
	}

	Write->LogwrtResult = LogwrtResult;
}

/*
 * Ensure that all XLOG data through the given position is flushed to disk.
 *
 * NOTE: this differs from XLogWrite mainly in that the WALWriteLock is not
 * already held, and we try to avoid acquiring it if possible.
 */
void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqstPtr;
	XLogwrtRqst WriteRqst;

	/* Disabled during REDO */
	if (InRedo)
		return;

	/* Quick exit if already known flushed */
	if (XLByteLE(record, LogwrtResult.Flush))
		return;

	if (XLOG_DEBUG)
		elog(LOG, "xlog flush request %X/%X; write %X/%X; flush %X/%X",
			 record.xlogid, record.xrecoff,
			 LogwrtResult.Write.xlogid, LogwrtResult.Write.xrecoff,
			 LogwrtResult.Flush.xlogid, LogwrtResult.Flush.xrecoff);

	START_CRIT_SECTION();

	/*
	 * Since fsync is usually a horribly expensive operation, we try to
	 * piggyback as much data as we can on each fsync: if we see any more
	 * data entered into the xlog buffer, we'll write and fsync that too,
	 * so that the final value of LogwrtResult.Flush is as large as
	 * possible. This gives us some chance of avoiding another fsync
	 * immediately after.
	 */

	/* initialize to given target; may increase below */
	WriteRqstPtr = record;

	/* read LogwrtResult and update local state */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile XLogCtlData *xlogctl = XLogCtl;

		SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
		if (XLByteLT(WriteRqstPtr, xlogctl->LogwrtRqst.Write))
			WriteRqstPtr = xlogctl->LogwrtRqst.Write;
		LogwrtResult = xlogctl->LogwrtResult;
		SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
	}

	/* done already? */
	if (!XLByteLE(record, LogwrtResult.Flush))
	{
		/* now wait for the write lock */
		LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
		LogwrtResult = XLogCtl->Write.LogwrtResult;
		if (!XLByteLE(record, LogwrtResult.Flush))
		{
			/* try to write/flush later additions to XLOG as well */
			if (LWLockConditionalAcquire(WALInsertLock, LW_EXCLUSIVE))
			{
				XLogCtlInsert *Insert = &XLogCtl->Insert;
				uint32		freespace = INSERT_FREESPACE(Insert);

				if (freespace < SizeOfXLogRecord)		/* buffer is full */
					WriteRqstPtr = XLogCtl->xlblocks[Insert->curridx];
				else
				{
					WriteRqstPtr = XLogCtl->xlblocks[Insert->curridx];
					WriteRqstPtr.xrecoff -= freespace;
				}
				LWLockRelease(WALInsertLock);
				WriteRqst.Write = WriteRqstPtr;
				WriteRqst.Flush = WriteRqstPtr;
			}
			else
			{
				WriteRqst.Write = WriteRqstPtr;
				WriteRqst.Flush = record;
			}
			XLogWrite(WriteRqst);
		}
		LWLockRelease(WALWriteLock);
	}

	END_CRIT_SECTION();

	/*
	 * If we still haven't flushed to the request point then we have a
	 * problem; most likely, the requested flush point is past end of
	 * XLOG. This has been seen to occur when a disk page has a corrupted
	 * LSN.
	 *
	 * Formerly we treated this as a PANIC condition, but that hurts the
	 * system's robustness rather than helping it: we do not want to take
	 * down the whole system due to corruption on one data page.  In
	 * particular, if the bad page is encountered again during recovery
	 * then we would be unable to restart the database at all!	(This
	 * scenario has actually happened in the field several times with 7.1
	 * releases. Note that we cannot get here while InRedo is true, but if
	 * the bad page is brought in and marked dirty during recovery then
	 * CreateCheckpoint will try to flush it at the end of recovery.)
	 *
	 * The current approach is to ERROR under normal conditions, but only
	 * WARNING during recovery, so that the system can be brought up even
	 * if there's a corrupt LSN.  Note that for calls from xact.c, the
	 * ERROR will be promoted to PANIC since xact.c calls this routine
	 * inside a critical section.  However, calls from bufmgr.c are not
	 * within critical sections and so we will not force a restart for a
	 * bad LSN on a data page.
	 */
	if (XLByteLT(LogwrtResult.Flush, record))
		elog(InRecovery ? WARNING : ERROR,
			 "xlog flush request %X/%X is not satisfied --- flushed only to %X/%X",
			 record.xlogid, record.xrecoff,
			 LogwrtResult.Flush.xlogid, LogwrtResult.Flush.xrecoff);
}

/*
 * Create a new XLOG file segment, or open a pre-existing one.
 *
 * log, seg: identify segment to be created/opened.
 *
 * *use_existent: if TRUE, OK to use a pre-existing file (else, any
 * pre-existing file will be deleted).	On return, TRUE if a pre-existing
 * file was used.
 *
 * use_lock: if TRUE, acquire ControlFileLock while moving file into
 * place.  This should be TRUE except during bootstrap log creation.  The
 * caller must *not* hold the lock at call.
 *
 * Returns FD of opened file.
 */
static int
XLogFileInit(uint32 log, uint32 seg,
			 bool *use_existent, bool use_lock)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	char		zbuffer[BLCKSZ];
	int			fd;
	int			nbytes;

	XLogFileName(path, log, seg);

	/*
	 * Try to use existent file (checkpoint maker may have created it
	 * already)
	 */
	if (*use_existent)
	{
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY | XLOG_SYNC_BIT,
						   S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			if (errno != ENOENT)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
								path, log, seg)));
		}
		else
			return (fd);
	}

	/*
	 * Initialize an empty (all zeroes) segment.  NOTE: it is possible
	 * that another process is doing the same thing.  If so, we will end
	 * up pre-creating an extra log segment.  That seems OK, and better
	 * than holding the lock throughout this lengthy process.
	 */
	snprintf(tmppath, MAXPGPATH, "%s/xlogtemp.%d",
			 XLogDir, (int) getpid());

	unlink(tmppath);

	/* do not use XLOG_SYNC_BIT here --- want to fsync only at end of fill */
	fd = BasicOpenFile(tmppath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	/*
	 * Zero-fill the file.	We have to do this the hard way to ensure that
	 * all the file space has really been allocated --- on platforms that
	 * allow "holes" in files, just seeking to the end doesn't allocate
	 * intermediate space.	This way, we know that we have all the space
	 * and (after the fsync below) that all the indirect blocks are down
	 * on disk.  Therefore, fdatasync(2) or O_DSYNC will be sufficient to
	 * sync future writes to the log file.
	 */
	MemSet(zbuffer, 0, sizeof(zbuffer));
	for (nbytes = 0; nbytes < XLogSegSize; nbytes += sizeof(zbuffer))
	{
		errno = 0;
		if ((int) write(fd, zbuffer, sizeof(zbuffer)) != (int) sizeof(zbuffer))
		{
			int			save_errno = errno;

			/*
			 * If we fail to make the file, delete it to release disk
			 * space
			 */
			unlink(tmppath);
			/* if write didn't set errno, assume problem is no disk space */
			errno = save_errno ? save_errno : ENOSPC;

			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tmppath)));
		}
	}

	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));

	close(fd);

	/*
	 * Now move the segment into place with its final name.
	 *
	 * If caller didn't want to use a pre-existing file, get rid of any
	 * pre-existing file.  Otherwise, cope with possibility that someone
	 * else has created the file while we were filling ours: if so, use
	 * ours to pre-create a future log segment.
	 */
	if (!InstallXLogFileSegment(log, seg, tmppath,
								*use_existent, XLOGfileslop,
								use_lock))
	{
		/* No need for any more future segments... */
		unlink(tmppath);
	}

	/* Set flag to tell caller there was no existent file */
	*use_existent = false;

	/* Now open original target segment (might not be file I just made) */
	fd = BasicOpenFile(path, O_RDWR | PG_BINARY | XLOG_SYNC_BIT,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
			errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
				   path, log, seg)));

	return (fd);
}

/*
 * Install a new XLOG segment file as a current or future log segment.
 *
 * This is used both to install a newly-created segment (which has a temp
 * filename while it's being created) and to recycle an old segment.
 *
 * log, seg: identify segment to install as (or first possible target).
 *
 * tmppath: initial name of file to install.  It will be renamed into place.
 *
 * find_free: if TRUE, install the new segment at the first empty log/seg
 * number at or after the passed numbers.  If FALSE, install the new segment
 * exactly where specified, deleting any existing segment file there.
 *
 * max_advance: maximum number of log/seg slots to advance past the starting
 * point.  Fail if no free slot is found in this range.  (Irrelevant if
 * find_free is FALSE.)
 *
 * use_lock: if TRUE, acquire ControlFileLock while moving file into
 * place.  This should be TRUE except during bootstrap log creation.  The
 * caller must *not* hold the lock at call.
 *
 * Returns TRUE if file installed, FALSE if not installed because of
 * exceeding max_advance limit.  (Any other kind of failure causes ereport().)
 */
static bool
InstallXLogFileSegment(uint32 log, uint32 seg, char *tmppath,
					   bool find_free, int max_advance,
					   bool use_lock)
{
	char		path[MAXPGPATH];
	struct stat stat_buf;

	XLogFileName(path, log, seg);

	/*
	 * We want to be sure that only one process does this at a time.
	 */
	if (use_lock)
		LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);

	if (!find_free)
	{
		/* Force installation: get rid of any pre-existing segment file */
		unlink(path);
	}
	else
	{
		/* Find a free slot to put it in */
		while (stat(path, &stat_buf) == 0)
		{
			if (--max_advance < 0)
			{
				/* Failed to find a free slot within specified range */
				if (use_lock)
					LWLockRelease(ControlFileLock);
				return false;
			}
			NextLogSeg(log, seg);
			XLogFileName(path, log, seg);
		}
	}

	/*
	 * Prefer link() to rename() here just to be really sure that we don't
	 * overwrite an existing logfile.  However, there shouldn't be one, so
	 * rename() is an acceptable substitute except for the truly paranoid.
	 */
#if HAVE_WORKING_LINK
	if (link(tmppath, path) < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not link file \"%s\" to \"%s\" (initialization of log file %u, segment %u): %m",
						tmppath, path, log, seg)));
	unlink(tmppath);
#else
	if (rename(tmppath, path) < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\" (initialization of log file %u, segment %u): %m",
						tmppath, path, log, seg)));
#endif

	if (use_lock)
		LWLockRelease(ControlFileLock);

	return true;
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
			ereport(LOG,
					(errcode_for_file_access(),
			errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
				   path, log, seg)));
			return (fd);
		}
		ereport(PANIC,
				(errcode_for_file_access(),
			errmsg("could not open file \"%s\" (log file %u, segment %u): %m",
				   path, log, seg)));
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

	XLByteToPrevSeg(endptr, _logId, _logSeg);
	if ((endptr.xrecoff - 1) % XLogSegSize >=
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
 *
 * endptr is current (or recent) end of xlog; this is used to determine
 * whether we want to recycle rather than delete no-longer-wanted log files.
 */
static void
MoveOfflineLogs(uint32 log, uint32 seg, XLogRecPtr endptr)
{
	uint32		endlogId;
	uint32		endlogSeg;
	DIR		   *xldir;
	struct dirent *xlde;
	char		lastoff[32];
	char		path[MAXPGPATH];

	XLByteToPrevSeg(endptr, endlogId, endlogSeg);

	xldir = AllocateDir(XLogDir);
	if (xldir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
			errmsg("could not open transaction log directory \"%s\": %m",
				   XLogDir)));

	sprintf(lastoff, "%08X%08X", log, seg);

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) == 16 &&
			strspn(xlde->d_name, "0123456789ABCDEF") == 16 &&
			strcmp(xlde->d_name, lastoff) <= 0)
		{
			snprintf(path, MAXPGPATH, "%s/%s", XLogDir, xlde->d_name);
			if (XLOG_archive_dir[0])
			{
				ereport(LOG,
						(errmsg("archiving transaction log file \"%s\"",
								xlde->d_name)));
				elog(WARNING, "archiving log files is not implemented");
			}
			else
			{
				/*
				 * Before deleting the file, see if it can be recycled as
				 * a future log segment.  We allow recycling segments up
				 * to XLOGfileslop segments beyond the current XLOG
				 * location.
				 */
				if (InstallXLogFileSegment(endlogId, endlogSeg, path,
										   true, XLOGfileslop,
										   true))
				{
					ereport(LOG,
						  (errmsg("recycled transaction log file \"%s\"",
								  xlde->d_name)));
				}
				else
				{
					/* No need for any more future segments... */
					ereport(LOG,
						  (errmsg("removing transaction log file \"%s\"",
								  xlde->d_name)));
					unlink(path);
				}
			}
		}
		errno = 0;
	}
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
			errmsg("could not read transaction log directory \"%s\": %m",
				   XLogDir)));
	FreeDir(xldir);
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

	blk = (char *) XLogRecGetData(record) + record->xl_len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;

		memcpy((char *) &bkpb, blk, sizeof(BkpBlock));
		blk += sizeof(BkpBlock);

		reln = XLogOpenRelation(true, record->xl_rmid, bkpb.node);

		if (reln)
		{
			buffer = XLogReadBuffer(true, reln, bkpb.block);
			if (BufferIsValid(buffer))
			{
				page = (Page) BufferGetPage(buffer);
				memcpy((char *) page, blk, BLCKSZ);
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
	COMP_CRC64(crc, (char *) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(record->xl_crc, crc))
	{
		ereport(emode,
		 (errmsg("incorrect resource manager data checksum in record at %X/%X",
				 recptr.xlogid, recptr.xrecoff)));
		return (false);
	}

	/* Check CRCs of backup blocks, if any */
	blk = (char *) XLogRecGetData(record) + len;
	for (i = 0; i < XLR_MAX_BKP_BLOCKS; i++)
	{
		if (!(record->xl_info & XLR_SET_BKP_BLOCK(i)))
			continue;

		INIT_CRC64(crc);
		COMP_CRC64(crc, blk + sizeof(BkpBlock), BLCKSZ);
		COMP_CRC64(crc, blk + sizeof(crc64),
				   sizeof(BkpBlock) - sizeof(crc64));
		FIN_CRC64(crc);
		memcpy((char *) &cbuf, blk, sizeof(crc64));		/* don't assume
														 * alignment */

		if (!EQ_CRC64(cbuf, crc))
		{
			ereport(emode,
			(errmsg("incorrect checksum of backup block %d in record at %X/%X",
					i + 1, recptr.xlogid, recptr.xrecoff)));
			return (false);
		}
		blk += sizeof(BkpBlock) + BLCKSZ;
	}

	return (true);
}

/*
 * Attempt to read an XLOG record.
 *
 * If RecPtr is not NULL, try to read a record at that position.  Otherwise
 * try to read a record just after the last one previously read.
 *
 * If no valid record is available, returns NULL, or fails if emode is PANIC.
 * (emode must be either PANIC or LOG.)
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
		 * reasons: (1) no need to waste the storage in most
		 * instantiations of the backend; (2) a static char array isn't
		 * guaranteed to have any particular alignment, whereas malloc()
		 * will provide MAXALIGN'd storage.
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
		ereport(PANIC,
				(errmsg("invalid record offset at %X/%X",
						RecPtr->xlogid, RecPtr->xrecoff)));

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
		readOff = (uint32) (-1);	/* force read to occur below */
	}

	targetPageOff = ((RecPtr->xrecoff % XLogSegSize) / BLCKSZ) * BLCKSZ;
	if (readOff != targetPageOff)
	{
		readOff = targetPageOff;
		if (lseek(readFile, (off_t) readOff, SEEK_SET) < 0)
		{
			ereport(emode,
					(errcode_for_file_access(),
					 errmsg("could not seek in log file %u, segment %u to offset %u: %m",
							readId, readSeg, readOff)));
			goto next_record_is_invalid;
		}
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
		{
			ereport(emode,
					(errcode_for_file_access(),
					 errmsg("could not read from log file %u, segment %u at offset %u: %m",
							readId, readSeg, readOff)));
			goto next_record_is_invalid;
		}
		if (!ValidXLOGHeader((XLogPageHeader) readBuf, emode, nextmode))
			goto next_record_is_invalid;
	}
	if ((((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD) &&
		RecPtr->xrecoff % BLCKSZ == SizeOfXLogPHD)
	{
		ereport(emode,
				(errmsg("contrecord is requested by %X/%X",
						RecPtr->xlogid, RecPtr->xrecoff)));
		goto next_record_is_invalid;
	}
	record = (XLogRecord *) ((char *) readBuf + RecPtr->xrecoff % BLCKSZ);

got_record:;

	/*
	 * Currently, xl_len == 0 must be bad data, but that might not be true
	 * forever.  See note in XLogInsert.
	 */
	if (record->xl_len == 0)
	{
		ereport(emode,
				(errmsg("record with zero length at %X/%X",
						RecPtr->xlogid, RecPtr->xrecoff)));
		goto next_record_is_invalid;
	}
	if (!nextmode)
	{
		/*
		 * We can't exactly verify the prev-link, but surely it should be
		 * less than the record's own address.
		 */
		if (!XLByteLT(record->xl_prev, *RecPtr))
		{
			ereport(emode,
					(errmsg("record with incorrect prev-link %X/%X at %X/%X",
							record->xl_prev.xlogid, record->xl_prev.xrecoff,
							RecPtr->xlogid, RecPtr->xrecoff)));
			goto next_record_is_invalid;
		}
	}
	else
	{
		/*
		 * Record's prev-link should exactly match our previous location.
		 * This check guards against torn WAL pages where a stale but
		 * valid-looking WAL record starts on a sector boundary.
		 */
		if (!XLByteEQ(record->xl_prev, ReadRecPtr))
		{
			ereport(emode,
					(errmsg("record with incorrect prev-link %X/%X at %X/%X",
							record->xl_prev.xlogid, record->xl_prev.xrecoff,
							RecPtr->xlogid, RecPtr->xrecoff)));
			goto next_record_is_invalid;
		}
	}

	/*
	 * Compute total length of record including any appended backup
	 * blocks.
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
		ereport(emode,
				(errmsg("record length %u at %X/%X too long",
						total_len, RecPtr->xlogid, RecPtr->xrecoff)));
		goto next_record_is_invalid;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		ereport(emode,
				(errmsg("invalid resource manager ID %u at %X/%X",
					 record->xl_rmid, RecPtr->xlogid, RecPtr->xrecoff)));
		goto next_record_is_invalid;
	}
	nextRecord = NULL;
	len = BLCKSZ - RecPtr->xrecoff % BLCKSZ;
	if (total_len > len)
	{
		/* Need to reassemble record */
		XLogContRecord *contrecord;
		uint32		gotlen = len;

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
				ereport(emode,
						(errcode_for_file_access(),
						 errmsg("could not read from log file %u, segment %u, offset %u: %m",
								readId, readSeg, readOff)));
				goto next_record_is_invalid;
			}
			if (!ValidXLOGHeader((XLogPageHeader) readBuf, emode, true))
				goto next_record_is_invalid;
			if (!(((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_CONTRECORD))
			{
				ereport(emode,
						(errmsg("there is no contrecord flag in log file %u, segment %u, offset %u",
								readId, readSeg, readOff)));
				goto next_record_is_invalid;
			}
			contrecord = (XLogContRecord *) ((char *) readBuf + SizeOfXLogPHD);
			if (contrecord->xl_rem_len == 0 ||
				total_len != (contrecord->xl_rem_len + gotlen))
			{
				ereport(emode,
						(errmsg("invalid contrecord length %u in log file %u, segment %u, offset %u",
								contrecord->xl_rem_len,
								readId, readSeg, readOff)));
				goto next_record_is_invalid;
			}
			len = BLCKSZ - SizeOfXLogPHD - SizeOfXLogContRecord;
			if (contrecord->xl_rem_len > len)
			{
				memcpy(buffer, (char *) contrecord + SizeOfXLogContRecord, len);
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
 * ReadRecord.	It's not intended for use from anywhere else.
 */
static bool
ValidXLOGHeader(XLogPageHeader hdr, int emode, bool checkSUI)
{
	XLogRecPtr	recaddr;

	if (hdr->xlp_magic != XLOG_PAGE_MAGIC)
	{
		ereport(emode,
				(errmsg("invalid magic number %04X in log file %u, segment %u, offset %u",
						hdr->xlp_magic, readId, readSeg, readOff)));
		return false;
	}
	if ((hdr->xlp_info & ~XLP_ALL_FLAGS) != 0)
	{
		ereport(emode,
				(errmsg("invalid info bits %04X in log file %u, segment %u, offset %u",
						hdr->xlp_info, readId, readSeg, readOff)));
		return false;
	}
	recaddr.xlogid = readId;
	recaddr.xrecoff = readSeg * XLogSegSize + readOff;
	if (!XLByteEQ(hdr->xlp_pageaddr, recaddr))
	{
		ereport(emode,
				(errmsg("unexpected pageaddr %X/%X in log file %u, segment %u, offset %u",
					 hdr->xlp_pageaddr.xlogid, hdr->xlp_pageaddr.xrecoff,
						readId, readSeg, readOff)));
		return false;
	}

	/*
	 * We disbelieve a SUI less than the previous page's SUI, or more than
	 * a few counts greater.  In theory as many as 512 shutdown checkpoint
	 * records could appear on a 32K-sized xlog page, so that's the most
	 * differential there could legitimately be.
	 *
	 * Note this check can only be applied when we are reading the next page
	 * in sequence, so ReadRecord passes a flag indicating whether to
	 * check.
	 */
	if (checkSUI)
	{
		if (hdr->xlp_sui < lastReadSUI ||
			hdr->xlp_sui > lastReadSUI + 512)
		{
			ereport(emode,
			/* translator: SUI = startup id */
					(errmsg("out-of-sequence SUI %u (after %u) in log file %u, segment %u, offset %u",
							hdr->xlp_sui, lastReadSUI,
							readId, readSeg, readOff)));
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
 * contents of pg_control.	WriteControlFile() initializes pg_control
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
	snprintf(XLogDir, MAXPGPATH, "%s/pg_xlog", DataDir);
	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ]; /* need not be aligned */
	char	   *localeptr;

	/*
	 * Initialize version and compatibility-check fields
	 */
	ControlFile->pg_control_version = PG_CONTROL_VERSION;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;
	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;

	ControlFile->nameDataLen = NAMEDATALEN;
	ControlFile->funcMaxArgs = FUNC_MAX_ARGS;

#ifdef HAVE_INT64_TIMESTAMP
	ControlFile->enableIntTimes = TRUE;
#else
	ControlFile->enableIntTimes = FALSE;
#endif

	ControlFile->localeBuflen = LOCALE_NAME_BUFLEN;
	localeptr = setlocale(LC_COLLATE, NULL);
	if (!localeptr)
		ereport(PANIC,
				(errmsg("invalid LC_COLLATE setting")));
	StrNCpy(ControlFile->lc_collate, localeptr, LOCALE_NAME_BUFLEN);
	localeptr = setlocale(LC_CTYPE, NULL);
	if (!localeptr)
		ereport(PANIC,
				(errmsg("invalid LC_CTYPE setting")));
	StrNCpy(ControlFile->lc_ctype, localeptr, LOCALE_NAME_BUFLEN);

	/* Contents are protected with a CRC */
	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc,
			   (char *) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile->crc);

	/*
	 * We write out BLCKSZ bytes into pg_control, zero-padding the excess
	 * over sizeof(ControlFileData).  This reduces the odds of
	 * premature-EOF errors when reading pg_control.  We'll still fail
	 * when we check the contents of the file, but hopefully with a more
	 * specific error than "couldn't read pg_control".
	 */
	if (sizeof(ControlFileData) > BLCKSZ)
		ereport(PANIC,
				(errmsg("sizeof(ControlFileData) is larger than BLCKSZ; fix either one")));

	memset(buffer, 0, BLCKSZ);
	memcpy(buffer, ControlFile, sizeof(ControlFileData));

	fd = BasicOpenFile(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create control file \"%s\": %m",
						ControlFilePath)));

	errno = 0;
	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to control file: %m")));
	}

	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync control file: %m")));

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
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open control file \"%s\": %m",
						ControlFilePath)));

	if (read(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not read from control file: %m")));

	close(fd);

	/*
	 * Check for expected pg_control format version.  If this is wrong,
	 * the CRC check will likely fail because we'll be checking the wrong
	 * number of bytes.  Complaining about wrong version will probably be
	 * more enlightening than complaining about wrong CRC.
	 */
	if (ControlFile->pg_control_version != PG_CONTROL_VERSION)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with PG_CONTROL_VERSION %d,"
			  " but the server was compiled with PG_CONTROL_VERSION %d.",
					ControlFile->pg_control_version, PG_CONTROL_VERSION),
				 errhint("It looks like you need to initdb.")));
	/* Now check the CRC. */
	INIT_CRC64(crc);
	COMP_CRC64(crc,
			   (char *) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(crc);

	if (!EQ_CRC64(crc, ControlFile->crc))
		ereport(FATAL,
				(errmsg("incorrect checksum in control file")));

	/*
	 * Do compatibility checking immediately.  We do this here for 2
	 * reasons:
	 *
	 * (1) if the database isn't compatible with the backend executable, we
	 * want to abort before we can possibly do any damage;
	 *
	 * (2) this code is executed in the postmaster, so the setlocale() will
	 * propagate to forked backends, which aren't going to read this file
	 * for themselves.	(These locale settings are considered critical
	 * compatibility items because they can affect sort order of indexes.)
	 */
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with CATALOG_VERSION_NO %d,"
			  " but the server was compiled with CATALOG_VERSION_NO %d.",
					ControlFile->catalog_version_no, CATALOG_VERSION_NO),
				 errhint("It looks like you need to initdb.")));
	if (ControlFile->blcksz != BLCKSZ)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
		 errdetail("The database cluster was initialized with BLCKSZ %d,"
				   " but the server was compiled with BLCKSZ %d.",
				   ControlFile->blcksz, BLCKSZ),
			 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->relseg_size != RELSEG_SIZE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with RELSEG_SIZE %d,"
					 " but the server was compiled with RELSEG_SIZE %d.",
						   ControlFile->relseg_size, RELSEG_SIZE),
			 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->nameDataLen != NAMEDATALEN)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with NAMEDATALEN %d,"
					 " but the server was compiled with NAMEDATALEN %d.",
						   ControlFile->nameDataLen, NAMEDATALEN),
			 errhint("It looks like you need to recompile or initdb.")));
	if (ControlFile->funcMaxArgs != FUNC_MAX_ARGS)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with FUNC_MAX_ARGS %d,"
				   " but the server was compiled with FUNC_MAX_ARGS %d.",
						   ControlFile->funcMaxArgs, FUNC_MAX_ARGS),
			 errhint("It looks like you need to recompile or initdb.")));

#ifdef HAVE_INT64_TIMESTAMP
	if (ControlFile->enableIntTimes != TRUE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized without HAVE_INT64_TIMESTAMP"
			  " but the server was compiled with HAVE_INT64_TIMESTAMP."),
			 errhint("It looks like you need to recompile or initdb.")));
#else
	if (ControlFile->enableIntTimes != FALSE)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with HAVE_INT64_TIMESTAMP"
		   " but the server was compiled without HAVE_INT64_TIMESTAMP."),
			 errhint("It looks like you need to recompile or initdb.")));
#endif

	if (ControlFile->localeBuflen != LOCALE_NAME_BUFLEN)
		ereport(FATAL,
				(errmsg("database files are incompatible with server"),
				 errdetail("The database cluster was initialized with LOCALE_NAME_BUFLEN %d,"
			  " but the server was compiled with LOCALE_NAME_BUFLEN %d.",
						   ControlFile->localeBuflen, LOCALE_NAME_BUFLEN),
			 errhint("It looks like you need to recompile or initdb.")));
	if (pg_perm_setlocale(LC_COLLATE, ControlFile->lc_collate) == NULL)
		ereport(FATAL,
		(errmsg("database files are incompatible with operating system"),
		 errdetail("The database cluster was initialized with LC_COLLATE \"%s\","
				   " which is not recognized by setlocale().",
				   ControlFile->lc_collate),
		 errhint("It looks like you need to initdb or install locale support.")));
	if (pg_perm_setlocale(LC_CTYPE, ControlFile->lc_ctype) == NULL)
		ereport(FATAL,
		(errmsg("database files are incompatible with operating system"),
		 errdetail("The database cluster was initialized with LC_CTYPE \"%s\","
				   " which is not recognized by setlocale().",
				   ControlFile->lc_ctype),
		 errhint("It looks like you need to initdb or install locale support.")));

	/* Make the fixed locale settings visible as GUC variables, too */
	SetConfigOption("lc_collate", ControlFile->lc_collate,
					PGC_INTERNAL, PGC_S_OVERRIDE);
	SetConfigOption("lc_ctype", ControlFile->lc_ctype,
					PGC_INTERNAL, PGC_S_OVERRIDE);
}

void
UpdateControlFile(void)
{
	int			fd;

	INIT_CRC64(ControlFile->crc);
	COMP_CRC64(ControlFile->crc,
			   (char *) ControlFile + sizeof(crc64),
			   sizeof(ControlFileData) - sizeof(crc64));
	FIN_CRC64(ControlFile->crc);

	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open control file \"%s\": %m",
						ControlFilePath)));

	errno = 0;
	if (write(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to control file: %m")));
	}

	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync control file: %m")));

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
	 * Here, on the other hand, we must MAXALIGN to ensure the page
	 * buffers have worst-case alignment.
	 */
	XLogCtl->pages =
		((char *) XLogCtl) + MAXALIGN(sizeof(XLogCtlData) +
									  sizeof(XLogRecPtr) * XLOGbuffers);
	memset(XLogCtl->pages, 0, BLCKSZ * XLOGbuffers);

	/*
	 * Do basic initialization of XLogCtl shared data. (StartupXLOG will
	 * fill in additional info.)
	 */
	XLogCtl->XLogCacheByte = BLCKSZ * XLOGbuffers;
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	XLogCtl->Insert.currpage = (XLogPageHeader) (XLogCtl->pages);
	SpinLockInit(&XLogCtl->info_lck);

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
	bool		use_existent;
	crc64		crc;

	/* Use malloc() to ensure buffer is MAXALIGNED */
	buffer = (char *) malloc(BLCKSZ);
	page = (XLogPageHeader) buffer;

	checkPoint.redo.xlogid = 0;
	checkPoint.redo.xrecoff = SizeOfXLogPHD;
	checkPoint.undo = checkPoint.redo;
	checkPoint.ThisStartUpID = 0;
	checkPoint.nextXid = FirstNormalTransactionId;
	checkPoint.nextOid = BootstrapObjectIdData;
	checkPoint.time = time(NULL);

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	page->xlp_sui = checkPoint.ThisStartUpID;
	page->xlp_pageaddr.xlogid = 0;
	page->xlp_pageaddr.xrecoff = 0;
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
	COMP_CRC64(crc, (char *) record + sizeof(crc64),
			   SizeOfXLogRecord - sizeof(crc64));
	FIN_CRC64(crc);
	record->xl_crc = crc;

	use_existent = false;
	openLogFile = XLogFileInit(0, 0, &use_existent, false);

	errno = 0;
	if (write(openLogFile, buffer, BLCKSZ) != BLCKSZ)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write bootstrap transaction log file: %m")));
	}

	if (pg_fsync(openLogFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync bootstrap transaction log file: %m")));

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

	/* Bootstrap the commit log, too */
	BootStrapCLOG();
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
	uint32		freespace;

	/* Use malloc() to ensure record buffer is MAXALIGNED */
	buffer = (char *) malloc(_INTL_MAXLOGRECSZ);

	CritSectionCount++;

	/*
	 * Read control file and check XLOG status looks valid.
	 *
	 * Note: in most control paths, *ControlFile is already valid and we need
	 * not do ReadControlFile() here, but might as well do it to be sure.
	 */
	ReadControlFile();

	if (ControlFile->logSeg == 0 ||
		ControlFile->state < DB_SHUTDOWNED ||
		ControlFile->state > DB_IN_PRODUCTION ||
		!XRecOffIsValid(ControlFile->checkPoint.xrecoff))
		ereport(FATAL,
				(errmsg("control file contains invalid data")));

	if (ControlFile->state == DB_SHUTDOWNED)
		ereport(LOG,
				(errmsg("database system was shut down at %s",
						str_time(ControlFile->time))));
	else if (ControlFile->state == DB_SHUTDOWNING)
		ereport(LOG,
				(errmsg("database system shutdown was interrupted at %s",
						str_time(ControlFile->time))));
	else if (ControlFile->state == DB_IN_RECOVERY)
		ereport(LOG,
		(errmsg("database system was interrupted while in recovery at %s",
				str_time(ControlFile->time)),
		 errhint("This probably means that some data is corrupted and"
				 " you will have to use the last backup for recovery.")));
	else if (ControlFile->state == DB_IN_PRODUCTION)
		ereport(LOG,
				(errmsg("database system was interrupted at %s",
						str_time(ControlFile->time))));

	/* This is just to allow attaching to startup process with a debugger */
#ifdef XLOG_REPLAY_DELAY
	if (XLOG_DEBUG && ControlFile->state != DB_SHUTDOWNED)
		sleep(60);
#endif

	/*
	 * Get the last valid checkpoint record.  If the latest one according
	 * to pg_control is broken, try the next-to-last one.
	 */
	record = ReadCheckpointRecord(ControlFile->checkPoint, 1, buffer);
	if (record != NULL)
	{
		checkPointLoc = ControlFile->checkPoint;
		ereport(LOG,
				(errmsg("checkpoint record is at %X/%X",
						checkPointLoc.xlogid, checkPointLoc.xrecoff)));
	}
	else
	{
		record = ReadCheckpointRecord(ControlFile->prevCheckPoint, 2, buffer);
		if (record != NULL)
		{
			checkPointLoc = ControlFile->prevCheckPoint;
			ereport(LOG,
					(errmsg("using previous checkpoint record at %X/%X",
						  checkPointLoc.xlogid, checkPointLoc.xrecoff)));
			InRecovery = true;	/* force recovery even if SHUTDOWNED */
		}
		else
			ereport(PANIC,
				 (errmsg("could not locate a valid checkpoint record")));
	}
	LastRec = RecPtr = checkPointLoc;
	memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
	wasShutdown = (record->xl_info == XLOG_CHECKPOINT_SHUTDOWN);

	ereport(LOG,
			(errmsg("redo record is at %X/%X; undo record is at %X/%X; shutdown %s",
					checkPoint.redo.xlogid, checkPoint.redo.xrecoff,
					checkPoint.undo.xlogid, checkPoint.undo.xrecoff,
					wasShutdown ? "TRUE" : "FALSE")));
	ereport(LOG,
			(errmsg("next transaction ID: %u; next OID: %u",
					checkPoint.nextXid, checkPoint.nextOid)));
	if (!TransactionIdIsNormal(checkPoint.nextXid))
		ereport(PANIC,
				(errmsg("invalid next transaction ID")));

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	/*
	 * If it was a shutdown checkpoint, then any following WAL entries
	 * were created under the next StartUpID; if it was a regular
	 * checkpoint then any following WAL entries were created under the
	 * same StartUpID. We must replay WAL entries using the same StartUpID
	 * they were created under, so temporarily adopt that SUI (see also
	 * xlog_redo()).
	 */
	if (wasShutdown)
		ThisStartUpID = checkPoint.ThisStartUpID + 1;
	else
		ThisStartUpID = checkPoint.ThisStartUpID;

	RedoRecPtr = XLogCtl->Insert.RedoRecPtr =
		XLogCtl->SavedRedoRecPtr = checkPoint.redo;

	if (XLByteLT(RecPtr, checkPoint.redo))
		ereport(PANIC,
				(errmsg("invalid redo in checkpoint record")));
	if (checkPoint.undo.xrecoff == 0)
		checkPoint.undo = RecPtr;

	if (XLByteLT(checkPoint.undo, RecPtr) ||
		XLByteLT(checkPoint.redo, RecPtr))
	{
		if (wasShutdown)
			ereport(PANIC,
			(errmsg("invalid redo/undo record in shutdown checkpoint")));
		InRecovery = true;
	}
	else if (ControlFile->state != DB_SHUTDOWNED)
		InRecovery = true;

	/* REDO */
	if (InRecovery)
	{
		int			rmid;

		ereport(LOG,
				(errmsg("database system was not properly shut down; "
						"automatic recovery in progress")));
		ControlFile->state = DB_IN_RECOVERY;
		ControlFile->time = time(NULL);
		UpdateControlFile();

		/* Start up the recovery environment */
		XLogInitRelationCache();

		for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
		{
			if (RmgrTable[rmid].rm_startup != NULL)
				RmgrTable[rmid].rm_startup();
		}

		/* Is REDO required ? */
		if (XLByteLT(checkPoint.redo, RecPtr))
			record = ReadRecord(&(checkPoint.redo), PANIC, buffer);
		else
		{
			/* read past CheckPoint record */
			record = ReadRecord(NULL, LOG, buffer);
		}

		if (record != NULL)
		{
			InRedo = true;
			ereport(LOG,
					(errmsg("redo starts at %X/%X",
							ReadRecPtr.xlogid, ReadRecPtr.xrecoff)));
			do
			{
				/* nextXid must be beyond record's xid */
				if (TransactionIdFollowsOrEquals(record->xl_xid,
											ShmemVariableCache->nextXid))
				{
					ShmemVariableCache->nextXid = record->xl_xid;
					TransactionIdAdvance(ShmemVariableCache->nextXid);
				}
				if (XLOG_DEBUG)
				{
					char		buf[8192];

					sprintf(buf, "REDO @ %X/%X; LSN %X/%X: ",
							ReadRecPtr.xlogid, ReadRecPtr.xrecoff,
							EndRecPtr.xlogid, EndRecPtr.xrecoff);
					xlog_outrec(buf, record);
					strcat(buf, " - ");
					RmgrTable[record->xl_rmid].rm_desc(buf,
								record->xl_info, XLogRecGetData(record));
					elog(LOG, "%s", buf);
				}

				if (record->xl_info & XLR_BKP_BLOCK_MASK)
					RestoreBkpBlocks(record, EndRecPtr);

				RmgrTable[record->xl_rmid].rm_redo(EndRecPtr, record);
				record = ReadRecord(NULL, LOG, buffer);
			} while (record != NULL);
			ereport(LOG,
					(errmsg("redo done at %X/%X",
							ReadRecPtr.xlogid, ReadRecPtr.xrecoff)));
			LastRec = ReadRecPtr;
			InRedo = false;
		}
		else
			ereport(LOG,
					(errmsg("redo is not required")));
	}

	/*
	 * Init xlog buffer cache using the block containing the last valid
	 * record from the previous incarnation.
	 */
	record = ReadRecord(&LastRec, PANIC, buffer);
	EndOfLog = EndRecPtr;
	XLByteToPrevSeg(EndOfLog, openLogId, openLogSeg);
	openLogFile = XLogFileOpen(openLogId, openLogSeg, false);
	openLogOff = 0;
	ControlFile->logId = openLogId;
	ControlFile->logSeg = openLogSeg + 1;
	Insert = &XLogCtl->Insert;
	Insert->PrevRecord = LastRec;
	XLogCtl->xlblocks[0].xlogid = openLogId;
	XLogCtl->xlblocks[0].xrecoff =
		((EndOfLog.xrecoff - 1) / BLCKSZ + 1) * BLCKSZ;

	/*
	 * Tricky point here: readBuf contains the *last* block that the
	 * LastRec record spans, not the one it starts in.	The last block is
	 * indeed the one we want to use.
	 */
	Assert(readOff == (XLogCtl->xlblocks[0].xrecoff - BLCKSZ) % XLogSegSize);
	memcpy((char *) Insert->currpage, readBuf, BLCKSZ);
	Insert->currpos = (char *) Insert->currpage +
		(EndOfLog.xrecoff + BLCKSZ - XLogCtl->xlblocks[0].xrecoff);

	LogwrtResult.Write = LogwrtResult.Flush = EndOfLog;

	XLogCtl->Write.LogwrtResult = LogwrtResult;
	Insert->LogwrtResult = LogwrtResult;
	XLogCtl->LogwrtResult = LogwrtResult;

	XLogCtl->LogwrtRqst.Write = EndOfLog;
	XLogCtl->LogwrtRqst.Flush = EndOfLog;

	freespace = INSERT_FREESPACE(Insert);
	if (freespace > 0)
	{
		/* Make sure rest of page is zero */
		MemSet(Insert->currpos, 0, freespace);
		XLogCtl->Write.curridx = 0;
	}
	else
	{
		/*
		 * Whenever Write.LogwrtResult points to exactly the end of a
		 * page, Write.curridx must point to the *next* page (see
		 * XLogWrite()).
		 *
		 * Note: it might seem we should do AdvanceXLInsertBuffer() here, but
		 * we can't since we haven't yet determined the correct StartUpID
		 * to put into the new page's header.  The first actual attempt to
		 * insert a log record will advance the insert state.
		 */
		XLogCtl->Write.curridx = NextBufIdx(0);
	}

#ifdef NOT_USED
	/* UNDO */
	if (InRecovery)
	{
		RecPtr = ReadRecPtr;
		if (XLByteLT(checkPoint.undo, RecPtr))
		{
			ereport(LOG,
					(errmsg("undo starts at %X/%X",
							RecPtr.xlogid, RecPtr.xrecoff)));
			do
			{
				record = ReadRecord(&RecPtr, PANIC, buffer);
				if (TransactionIdIsValid(record->xl_xid) &&
					!TransactionIdDidCommit(record->xl_xid))
					RmgrTable[record->xl_rmid].rm_undo(EndRecPtr, record);
				RecPtr = record->xl_prev;
			} while (XLByteLE(checkPoint.undo, RecPtr));
			ereport(LOG,
					(errmsg("undo done at %X/%X",
							ReadRecPtr.xlogid, ReadRecPtr.xrecoff)));
		}
		else
			ereport(LOG,
					(errmsg("undo is not required")));
	}
#endif

	if (InRecovery)
	{
		int			rmid;

		/*
		 * Allow resource managers to do any required cleanup.
		 */
		for (rmid = 0; rmid <= RM_MAX_ID; rmid++)
		{
			if (RmgrTable[rmid].rm_cleanup != NULL)
				RmgrTable[rmid].rm_cleanup();
		}

		/* suppress in-transaction check in CreateCheckPoint */
		MyLastRecPtr.xrecoff = 0;
		MyXactMadeXLogEntry = false;
		MyXactMadeTempRelUpdate = false;

		/*
		 * At this point, ThisStartUpID is the largest SUI that we could
		 * find evidence for in the WAL entries.  But check it against
		 * pg_control's latest checkpoint, to make sure that we can't
		 * accidentally re-use an already-used SUI.
		 */
		if (ThisStartUpID < ControlFile->checkPointCopy.ThisStartUpID)
			ThisStartUpID = ControlFile->checkPointCopy.ThisStartUpID;

		/*
		 * Perform a new checkpoint to update our recovery activity to
		 * disk.
		 *
		 * Note that we write a shutdown checkpoint.  This is correct since
		 * the records following it will use SUI one more than what is
		 * shown in the checkpoint's ThisStartUpID.
		 *
		 * In case we had to use the secondary checkpoint, make sure that it
		 * will still be shown as the secondary checkpoint after this
		 * CreateCheckPoint operation; we don't want the broken primary
		 * checkpoint to become prevCheckPoint...
		 */
		ControlFile->checkPoint = checkPointLoc;
		CreateCheckPoint(true, true);

		/*
		 * Close down recovery environment
		 */
		XLogCloseRelationCache();
	}
	else
	{
		/*
		 * If we are not doing recovery, then we saw a checkpoint with
		 * nothing after it, and we can safely use StartUpID equal to one
		 * more than the checkpoint's SUI.  But just for paranoia's sake,
		 * check against pg_control too.
		 */
		ThisStartUpID = checkPoint.ThisStartUpID;
		if (ThisStartUpID < ControlFile->checkPointCopy.ThisStartUpID)
			ThisStartUpID = ControlFile->checkPointCopy.ThisStartUpID;
	}

	/*
	 * Preallocate additional log files, if wanted.
	 */
	PreallocXlogFiles(EndOfLog);

	/*
	 * Advance StartUpID to one more than the highest value used
	 * previously.
	 */
	ThisStartUpID++;
	XLogCtl->ThisStartUpID = ThisStartUpID;

	/*
	 * Okay, we're officially UP.
	 */
	InRecovery = false;

	ControlFile->state = DB_IN_PRODUCTION;
	ControlFile->time = time(NULL);
	UpdateControlFile();

	/* Start up the commit log, too */
	StartupCLOG();

	ereport(LOG,
			(errmsg("database system is ready")));
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

/*
 * Subroutine to try to fetch and validate a prior checkpoint record.
 * whichChkpt = 1 for "primary", 2 for "secondary", merely informative
 */
static XLogRecord *
ReadCheckpointRecord(XLogRecPtr RecPtr,
					 int whichChkpt,
					 char *buffer)
{
	XLogRecord *record;

	if (!XRecOffIsValid(RecPtr.xrecoff))
	{
		if (whichChkpt == 1)
			ereport(LOG,
					(errmsg("invalid primary checkpoint link in control file")));
		else
			ereport(LOG,
					(errmsg("invalid secondary checkpoint link in control file")));
		return NULL;
	}

	record = ReadRecord(&RecPtr, LOG, buffer);

	if (record == NULL)
	{
		if (whichChkpt == 1)
			ereport(LOG,
					(errmsg("invalid primary checkpoint record")));
		else
			ereport(LOG,
					(errmsg("invalid secondary checkpoint record")));
		return NULL;
	}
	if (record->xl_rmid != RM_XLOG_ID)
	{
		if (whichChkpt == 1)
			ereport(LOG,
					(errmsg("invalid resource manager ID in primary checkpoint record")));
		else
			ereport(LOG,
					(errmsg("invalid resource manager ID in secondary checkpoint record")));
		return NULL;
	}
	if (record->xl_info != XLOG_CHECKPOINT_SHUTDOWN &&
		record->xl_info != XLOG_CHECKPOINT_ONLINE)
	{
		if (whichChkpt == 1)
			ereport(LOG,
					(errmsg("invalid xl_info in primary checkpoint record")));
		else
			ereport(LOG,
					(errmsg("invalid xl_info in secondary checkpoint record")));
		return NULL;
	}
	if (record->xl_len != sizeof(CheckPoint))
	{
		if (whichChkpt == 1)
			ereport(LOG,
					(errmsg("invalid length of primary checkpoint record")));
		else
			ereport(LOG,
					(errmsg("invalid length of secondary checkpoint record")));
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
	RedoRecPtr = XLogCtl->SavedRedoRecPtr;
}

/*
 * CheckPoint process called by postmaster saves copy of new RedoRecPtr
 * in shmem (using SetSavedRedoRecPtr).  When checkpointer completes,
 * postmaster calls GetSavedRedoRecPtr to update its own copy of RedoRecPtr,
 * so that subsequently-spawned backends will start out with a reasonably
 * up-to-date local RedoRecPtr.  Since these operations are not protected by
 * any lock and copying an XLogRecPtr isn't atomic, it's unsafe to use either
 * of these routines at other times!
 */
void
SetSavedRedoRecPtr(void)
{
	XLogCtl->SavedRedoRecPtr = RedoRecPtr;
}

void
GetSavedRedoRecPtr(void)
{
	RedoRecPtr = XLogCtl->SavedRedoRecPtr;
}

/*
 * Once spawned, a backend may update its local RedoRecPtr from
 * XLogCtl->Insert.RedoRecPtr; it must hold the insert lock or info_lck
 * to do so.  This is done in XLogInsert() or GetRedoRecPtr().
 */
XLogRecPtr
GetRedoRecPtr(void)
{
	/* use volatile pointer to prevent code rearrangement */
	volatile XLogCtlData *xlogctl = XLogCtl;

	SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
	Assert(XLByteLE(RedoRecPtr, xlogctl->Insert.RedoRecPtr));
	RedoRecPtr = xlogctl->Insert.RedoRecPtr;
	SpinLockRelease_NoHoldoff(&xlogctl->info_lck);

	return RedoRecPtr;
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownXLOG(void)
{
	ereport(LOG,
			(errmsg("shutting down")));

	/* suppress in-transaction check in CreateCheckPoint */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;
	MyXactMadeTempRelUpdate = false;

	CritSectionCount++;
	CreateDummyCaches();
	CreateCheckPoint(true, true);
	ShutdownCLOG();
	CritSectionCount--;

	ereport(LOG,
			(errmsg("database system is shut down")));
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 *
 * If force is true, we force a checkpoint regardless of whether any XLOG
 * activity has occurred since the last one.
 */
void
CreateCheckPoint(bool shutdown, bool force)
{
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogRecData rdata;
	uint32		freespace;
	uint32		_logId;
	uint32		_logSeg;

	if (MyXactMadeXLogEntry)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		  errmsg("checkpoint cannot be made inside transaction block")));

	/*
	 * Acquire CheckpointLock to ensure only one checkpoint happens at a
	 * time.
	 *
	 * The CheckpointLock can be held for quite a while, which is not good
	 * because we won't respond to a cancel/die request while waiting for
	 * an LWLock.  (But the alternative of using a regular lock won't work
	 * for background checkpoint processes, which are not regular
	 * backends.)  So, rather than use a plain LWLockAcquire, use this
	 * kluge to allow an interrupt to be accepted while we are waiting:
	 */
	while (!LWLockConditionalAcquire(CheckpointLock, LW_EXCLUSIVE))
	{
		CHECK_FOR_INTERRUPTS();
		sleep(1);
	}

	/*
	 * Use a critical section to force system panic if we have trouble.
	 */
	START_CRIT_SECTION();

	if (shutdown)
	{
		ControlFile->state = DB_SHUTDOWNING;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}

	MemSet(&checkPoint, 0, sizeof(checkPoint));
	checkPoint.ThisStartUpID = ThisStartUpID;
	checkPoint.time = time(NULL);

	/*
	 * We must hold CheckpointStartLock while determining the checkpoint
	 * REDO pointer.  This ensures that any concurrent transaction commits
	 * will be either not yet logged, or logged and recorded in pg_clog.
	 * See notes in RecordTransactionCommit().
	 */
	LWLockAcquire(CheckpointStartLock, LW_EXCLUSIVE);

	/* And we need WALInsertLock too */
	LWLockAcquire(WALInsertLock, LW_EXCLUSIVE);

	/*
	 * If this isn't a shutdown or forced checkpoint, and we have not
	 * inserted any XLOG records since the start of the last checkpoint,
	 * skip the checkpoint.  The idea here is to avoid inserting duplicate
	 * checkpoints when the system is idle. That wastes log space, and
	 * more importantly it exposes us to possible loss of both current and
	 * previous checkpoint records if the machine crashes just as we're
	 * writing the update. (Perhaps it'd make even more sense to
	 * checkpoint only when the previous checkpoint record is in a
	 * different xlog page?)
	 *
	 * We have to make two tests to determine that nothing has happened since
	 * the start of the last checkpoint: current insertion point must
	 * match the end of the last checkpoint record, and its redo pointer
	 * must point to itself.
	 */
	if (!shutdown && !force)
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
			LWLockRelease(WALInsertLock);
			LWLockRelease(CheckpointStartLock);
			LWLockRelease(CheckpointLock);
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
	 * after the checkpoint, even though physically before it.	Got that?
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
	 * this must be done while holding the insert lock AND the info_lck.
	 *
	 * Note: if we fail to complete the checkpoint, RedoRecPtr will be left
	 * pointing past where it really needs to point.  This is okay; the
	 * only consequence is that XLogInsert might back up whole buffers
	 * that it didn't really need to.  We can't postpone advancing
	 * RedoRecPtr because XLogInserts that happen while we are dumping
	 * buffers must assume that their buffer changes are not included in
	 * the checkpoint.
	 */
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile XLogCtlData *xlogctl = XLogCtl;

		SpinLockAcquire_NoHoldoff(&xlogctl->info_lck);
		RedoRecPtr = xlogctl->Insert.RedoRecPtr = checkPoint.redo;
		SpinLockRelease_NoHoldoff(&xlogctl->info_lck);
	}

	/*
	 * Get UNDO record ptr - this is oldest of PGPROC->logRec values. We
	 * do this while holding insert lock to ensure that we won't miss any
	 * about-to-commit transactions (UNDO must include all xacts that have
	 * commits after REDO point).
	 *
	 * XXX temporarily ifdef'd out to avoid three-way deadlock condition:
	 * GetUndoRecPtr needs to grab SInvalLock to ensure that it is looking
	 * at a stable set of proc records, but grabbing SInvalLock while
	 * holding WALInsertLock is no good.  GetNewTransactionId may cause a
	 * WAL record to be written while holding XidGenLock, and
	 * GetSnapshotData needs to get XidGenLock while holding SInvalLock,
	 * so there's a risk of deadlock. Need to find a better solution.  See
	 * pgsql-hackers discussion of 17-Dec-01.
	 */
#ifdef NOT_USED
	checkPoint.undo = GetUndoRecPtr();

	if (shutdown && checkPoint.undo.xrecoff != 0)
		elog(PANIC, "active transaction while database system is shutting down");
#endif

	/*
	 * Now we can release insert lock and checkpoint start lock, allowing
	 * other xacts to proceed even while we are flushing disk buffers.
	 */
	LWLockRelease(WALInsertLock);

	LWLockRelease(CheckpointStartLock);

	/*
	 * Get the other info we need for the checkpoint record.
	 */
	LWLockAcquire(XidGenLock, LW_SHARED);
	checkPoint.nextXid = ShmemVariableCache->nextXid;
	LWLockRelease(XidGenLock);

	LWLockAcquire(OidGenLock, LW_SHARED);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;
	LWLockRelease(OidGenLock);

	/*
	 * Having constructed the checkpoint record, ensure all shmem disk
	 * buffers and commit-log buffers are flushed to disk.
	 *
	 * This I/O could fail for various reasons.  If so, we will fail to
	 * complete the checkpoint, but there is no reason to force a system
	 * panic.  Accordingly, exit critical section while doing it.
	 */
	END_CRIT_SECTION();

	CheckPointCLOG();
	FlushBufferPool();

	START_CRIT_SECTION();

	/*
	 * Now insert the checkpoint record into XLOG.
	 */
	rdata.buffer = InvalidBuffer;
	rdata.data = (char *) (&checkPoint);
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
		ereport(PANIC,
				(errmsg("concurrent transaction log activity while database system is shutting down")));

	/*
	 * Select point at which we can truncate the log, which we base on the
	 * prior checkpoint's earliest info.
	 *
	 * With UNDO support: oldest item is redo or undo, whichever is older;
	 * but watch out for case that undo = 0.
	 *
	 * Without UNDO support: just use the redo pointer.  This allows xlog
	 * space to be freed much faster when there are long-running
	 * transactions.
	 */
#ifdef NOT_USED
	if (ControlFile->checkPointCopy.undo.xrecoff != 0 &&
		XLByteLT(ControlFile->checkPointCopy.undo,
				 ControlFile->checkPointCopy.redo))
		XLByteToSeg(ControlFile->checkPointCopy.undo, _logId, _logSeg);
	else
#endif
		XLByteToSeg(ControlFile->checkPointCopy.redo, _logId, _logSeg);

	/*
	 * Update the control file.
	 */
	LWLockAcquire(ControlFileLock, LW_EXCLUSIVE);
	if (shutdown)
		ControlFile->state = DB_SHUTDOWNED;
	ControlFile->prevCheckPoint = ControlFile->checkPoint;
	ControlFile->checkPoint = ProcLastRecPtr;
	ControlFile->checkPointCopy = checkPoint;
	ControlFile->time = time(NULL);
	UpdateControlFile();
	LWLockRelease(ControlFileLock);

	/*
	 * We are now done with critical updates; no need for system panic if
	 * we have trouble while fooling with offline log segments.
	 */
	END_CRIT_SECTION();

	/*
	 * Delete offline log files (those no longer needed even for previous
	 * checkpoint).
	 */
	if (_logId || _logSeg)
	{
		PrevLogSeg(_logId, _logSeg);
		MoveOfflineLogs(_logId, _logSeg, recptr);
	}

	/*
	 * Make more log segments if needed.  (Do this after deleting offline
	 * log segments, to avoid having peak disk space usage higher than
	 * necessary.)
	 */
	if (!shutdown)
		PreallocXlogFiles(recptr);

	LWLockRelease(CheckpointLock);
}

/*
 * Write a NEXTOID log record
 */
void
XLogPutNextOid(Oid nextOid)
{
	XLogRecData rdata;

	rdata.buffer = InvalidBuffer;
	rdata.data = (char *) (&nextOid);
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
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

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
		ShmemVariableCache->nextOid = checkPoint.nextOid;
		ShmemVariableCache->oidCount = 0;
		/* Any later WAL records should be run with shutdown SUI plus 1 */
		ThisStartUpID = checkPoint.ThisStartUpID + 1;
	}
	else if (info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint	checkPoint;

		memcpy(&checkPoint, XLogRecGetData(record), sizeof(CheckPoint));
		/* In an ONLINE checkpoint, treat the counters like NEXTOID */
		if (TransactionIdPrecedes(ShmemVariableCache->nextXid,
								  checkPoint.nextXid))
			ShmemVariableCache->nextXid = checkPoint.nextXid;
		if (ShmemVariableCache->nextOid < checkPoint.nextOid)
		{
			ShmemVariableCache->nextOid = checkPoint.nextOid;
			ShmemVariableCache->oidCount = 0;
		}
		/* Any later WAL records should be run with the then-active SUI */
		ThisStartUpID = checkPoint.ThisStartUpID;
	}
}

void
xlog_undo(XLogRecPtr lsn, XLogRecord *record)
{
}

void
xlog_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_CHECKPOINT_SHUTDOWN ||
		info == XLOG_CHECKPOINT_ONLINE)
	{
		CheckPoint *checkpoint = (CheckPoint *) rec;

		sprintf(buf + strlen(buf), "checkpoint: redo %X/%X; undo %X/%X; "
				"sui %u; xid %u; oid %u; %s",
				checkpoint->redo.xlogid, checkpoint->redo.xrecoff,
				checkpoint->undo.xlogid, checkpoint->undo.xrecoff,
				checkpoint->ThisStartUpID, checkpoint->nextXid,
				checkpoint->nextOid,
			 (info == XLOG_CHECKPOINT_SHUTDOWN) ? "shutdown" : "online");
	}
	else if (info == XLOG_NEXTOID)
	{
		Oid			nextOid;

		memcpy(&nextOid, rec, sizeof(Oid));
		sprintf(buf + strlen(buf), "nextOid: %u", nextOid);
	}
	else
		strcat(buf, "UNKNOWN");
}

static void
xlog_outrec(char *buf, XLogRecord *record)
{
	int			bkpb;
	int			i;

	sprintf(buf + strlen(buf), "prev %X/%X; xprev %X/%X; xid %u",
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
 * GUC support
 */
const char *
assign_xlog_sync_method(const char *method, bool doit, bool interactive)
{
	int			new_sync_method;
	int			new_sync_bit;

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
		return NULL;

	if (!doit)
		return method;

	if (sync_method != new_sync_method || open_sync_bit != new_sync_bit)
	{
		/*
		 * To ensure that no blocks escape unsynced, force an fsync on the
		 * currently open log segment (if any).  Also, if the open flag is
		 * changing, close the log file so it will be reopened (with new
		 * flag bit) at next use.
		 */
		if (openLogFile >= 0)
		{
			if (pg_fsync(openLogFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
					errmsg("could not fsync log file %u, segment %u: %m",
						   openLogId, openLogSeg)));
			if (open_sync_bit != new_sync_bit)
			{
				if (close(openLogFile) != 0)
					ereport(PANIC,
							(errcode_for_file_access(),
					errmsg("could not close log file %u, segment %u: %m",
						   openLogId, openLogSeg)));
				openLogFile = -1;
			}
		}
		sync_method = new_sync_method;
		open_sync_bit = new_sync_bit;
	}

	return method;
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
				ereport(PANIC,
						(errcode_for_file_access(),
					errmsg("could not fsync log file %u, segment %u: %m",
						   openLogId, openLogSeg)));
			break;
#ifdef HAVE_FDATASYNC
		case SYNC_METHOD_FDATASYNC:
			if (pg_fdatasync(openLogFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
				errmsg("could not fdatasync log file %u, segment %u: %m",
					   openLogId, openLogSeg)));
			break;
#endif
		case SYNC_METHOD_OPEN:
			/* write synced it already */
			break;
		default:
			elog(PANIC, "unrecognized wal_sync_method: %d", sync_method);
			break;
	}
}
