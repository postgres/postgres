/*-------------------------------------------------------------------------
 *
 * xlog.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/access/transam/xlog.c,v 1.39 2000/12/03 10:27:26 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
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
#include "storage/sinval.h"
#include "storage/proc.h"
#include "storage/spin.h"
#include "storage/s_lock.h"
#include "access/xlog.h"
#include "access/xlogutils.h"
#include "utils/builtins.h"
#include "utils/relcache.h"

#include "miscadmin.h"

int			XLOGbuffers = 8;
XLogRecPtr	MyLastRecPtr = {0, 0};
uint32		StopIfError = 0;
bool		InRecovery = false;
StartUpID	ThisStartUpID = 0;

int			XLOG_DEBUG = 0;

/* To read/update control file and create new log file */
SPINLOCK	ControlFileLockId;

/* To generate new xid */
SPINLOCK	XidGenLockId;

static char		XLogDir[MAXPGPATH];
static char		ControlFilePath[MAXPGPATH];

#define MinXLOGbuffers	4

typedef struct XLgwrRqst
{
	XLogRecPtr	Write;			/* byte (1-based) to write out */
	XLogRecPtr	Flush;			/* byte (1-based) to flush */
} XLgwrRqst;

typedef struct XLgwrResult
{
	XLogRecPtr	Write;			/* bytes written out */
	XLogRecPtr	Flush;			/* bytes flushed */
} XLgwrResult;

typedef struct XLogCtlInsert
{
	XLgwrResult LgwrResult;
	XLogRecPtr	PrevRecord;
	uint16		curridx;		/* current block index in cache */
	XLogPageHeader currpage;
	char	   *currpos;
} XLogCtlInsert;

typedef struct XLogCtlWrite
{
	XLgwrResult LgwrResult;
	uint16		curridx;		/* index of next block to write */
} XLogCtlWrite;


typedef struct XLogCtlData
{
	XLogCtlInsert	Insert;
	XLgwrRqst		LgwrRqst;
	XLgwrResult		LgwrResult;
	XLogCtlWrite	Write;
	char		   *pages;
	XLogRecPtr	   *xlblocks;		/* 1st byte ptr-s + BLCKSZ */
	uint32			XLogCacheByte;
	uint32			XLogCacheBlck;
	StartUpID		ThisStartUpID;
	slock_t			insert_lck;
	slock_t			info_lck;
	slock_t			lgwr_lck;
	slock_t			chkp_lck;		/* checkpoint lock */
} XLogCtlData;

static XLogCtlData *XLogCtl = NULL;

/*
 * Contents of pg_control
 */

typedef enum DBState
{
	DB_STARTUP = 0,
	DB_SHUTDOWNED,
	DB_SHUTDOWNING,
	DB_IN_RECOVERY,
	DB_IN_PRODUCTION
} DBState;

#define LOCALE_NAME_BUFLEN  128

typedef struct ControlFileData
{
	/*
	 * XLOG state
	 */
	uint32		logId;			/* current log file id */
	uint32		logSeg;			/* current log file segment (1-based) */
	XLogRecPtr	checkPoint;		/* last check point record ptr */
	time_t		time;			/* time stamp of last modification */
	DBState		state;			/* see enum above */

	/*
	 * this data is used to make sure that configuration of this DB is
	 * compatible with the backend executable
	 */
	uint32		blcksz;			/* block size for this DB */
	uint32		relseg_size;	/* blocks per segment of large relation */
	uint32		catalog_version_no;		/* internal version number */
	/* active locales --- "C" if compiled without USE_LOCALE: */
	char		lc_collate[LOCALE_NAME_BUFLEN];
	char		lc_ctype[LOCALE_NAME_BUFLEN];

	/*
	 * important directory locations
	 */
	char		archdir[MAXPGPATH];		/* where to move offline log files */
} ControlFileData;

static ControlFileData *ControlFile = NULL;


typedef struct CheckPoint
{
	XLogRecPtr		redo;		/* next RecPtr available when we */
								/* began to create CheckPoint */
								/* (i.e. REDO start point) */
	XLogRecPtr		undo;		/* first record of oldest in-progress */
								/* transaction when we started */
								/* (i.e. UNDO end point) */
	StartUpID		ThisStartUpID;
	TransactionId	nextXid;
	Oid				nextOid;
	bool			Shutdown;
} CheckPoint;

#define XLOG_CHECKPOINT		0x00
#define XLOG_NEXTOID		0x10

/*
 * We break each log file in 16Mb segments
 */
#define XLogSegSize		(16*1024*1024)
#define XLogLastSeg		(0xffffffff / XLogSegSize)
#define XLogFileSize	(XLogLastSeg * XLogSegSize)

#define XLogFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%c%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

#define XLogTempFileName(path, log, seg)	\
			snprintf(path, MAXPGPATH, "%s%cT%08X%08X",	\
					 XLogDir, SEP_CHAR, log, seg)

#define PrevBufIdx(curridx)		\
		((curridx == 0) ? XLogCtl->XLogCacheBlck : (curridx - 1))

#define NextBufIdx(curridx)		\
		((curridx == XLogCtl->XLogCacheBlck) ? 0 : (curridx + 1))

#define InitXLBuffer(curridx)	(\
				XLogCtl->xlblocks[curridx].xrecoff = \
				(XLogCtl->xlblocks[Insert->curridx].xrecoff == XLogFileSize) ? \
				BLCKSZ : (XLogCtl->xlblocks[Insert->curridx].xrecoff + BLCKSZ), \
				XLogCtl->xlblocks[curridx].xlogid = \
				(XLogCtl->xlblocks[Insert->curridx].xrecoff == XLogFileSize) ? \
				(XLogCtl->xlblocks[Insert->curridx].xlogid + 1) : \
				XLogCtl->xlblocks[Insert->curridx].xlogid, \
				Insert->curridx = curridx, \
				Insert->currpage = (XLogPageHeader) (XLogCtl->pages + curridx * BLCKSZ), \
				Insert->currpos = \
					((char*) Insert->currpage) + SizeOfXLogPHD, \
				Insert->currpage->xlp_magic = XLOG_PAGE_MAGIC, \
				Insert->currpage->xlp_info = 0 \
				)

#define XRecOffIsValid(xrecoff) \
		(xrecoff % BLCKSZ >= SizeOfXLogPHD && \
		(BLCKSZ - xrecoff % BLCKSZ) >= SizeOfXLogRecord)

static void GetFreeXLBuffer(void);
static void XLogWrite(char *buffer);
static int	XLogFileInit(uint32 log, uint32 seg, bool *usexistent);
static int	XLogFileOpen(uint32 log, uint32 seg, bool econt);
static XLogRecord *ReadRecord(XLogRecPtr *RecPtr, char *buffer);
static void WriteControlFile(void);
static void ReadControlFile(void);
static char *str_time(time_t tnow);
static void xlog_outrec(char *buf, XLogRecord *record);

static XLgwrResult LgwrResult = {{0, 0}, {0, 0}};
static XLgwrRqst LgwrRqst = {{0, 0}, {0, 0}};

static int	logFile = -1;
static uint32 logId = 0;
static uint32 logSeg = 0;
static uint32 logOff = 0;

static XLogRecPtr ReadRecPtr;
static XLogRecPtr EndRecPtr;
static int	readFile = -1;
static uint32 readId = 0;
static uint32 readSeg = 0;
static uint32 readOff = 0;
static char readBuf[BLCKSZ];
static XLogRecord *nextRecord = NULL;

static bool InRedo = false;

XLogRecPtr
XLogInsert(RmgrId rmid, uint8 info, char *hdr, uint32 hdrlen, char *buf, uint32 buflen)
{
	XLogCtlInsert  *Insert = &XLogCtl->Insert;
	XLogRecord	   *record;
	XLogSubRecord  *subrecord;
	XLogRecPtr		RecPtr;
	uint32			len = hdrlen + buflen,
					freespace,
					wlen;
	uint16			curridx;
	bool			updrqst = false;
	bool			no_tran = (rmid == RM_XLOG_ID) ? true : false;

	if (info & XLR_INFO_MASK)
	{
		if ((info & XLR_INFO_MASK) != XLOG_NO_TRAN)
			elog(STOP, "XLogInsert: invalid info mask %02X", 
				(info & XLR_INFO_MASK));
		no_tran = true;
		info &= ~XLR_INFO_MASK;
	}

	if (len == 0 || len > MAXLOGRECSZ)
		elog(STOP, "XLogInsert: invalid record len %u", len);

	if (IsBootstrapProcessingMode() && rmid != RM_XLOG_ID)
	{
		RecPtr.xlogid = 0;
		RecPtr.xrecoff = SizeOfXLogPHD;	/* start of 1st checkpoint record */
		return (RecPtr);
	}

	START_CRIT_CODE;

	/* obtain xlog insert lock */
	if (TAS(&(XLogCtl->insert_lck)))	/* busy */
	{
		bool		do_lgwr = true;
		unsigned	i = 0;

		for (;;)
		{
			/* try to read LgwrResult while waiting for insert lock */
			if (!TAS(&(XLogCtl->info_lck)))
			{
				LgwrRqst = XLogCtl->LgwrRqst;
				LgwrResult = XLogCtl->LgwrResult;
				S_UNLOCK(&(XLogCtl->info_lck));

				/*
				 * If cache is half filled then try to acquire lgwr lock
				 * and do LGWR work, but only once.
				 */
				if (do_lgwr &&
					(LgwrRqst.Write.xlogid != LgwrResult.Write.xlogid ||
					 (LgwrRqst.Write.xrecoff - LgwrResult.Write.xrecoff >=
					  XLogCtl->XLogCacheByte / 2)))
				{
					if (!TAS(&(XLogCtl->lgwr_lck)))
					{
						LgwrResult = XLogCtl->Write.LgwrResult;
						if (!TAS(&(XLogCtl->info_lck)))
						{
							LgwrRqst = XLogCtl->LgwrRqst;
							S_UNLOCK(&(XLogCtl->info_lck));
						}
						if (XLByteLT(LgwrResult.Write, LgwrRqst.Write))
						{
							XLogWrite(NULL);
							do_lgwr = false;
						}
						S_UNLOCK(&(XLogCtl->lgwr_lck));
					}
				}
			}
			s_lock_sleep(i++);
			if (!TAS(&(XLogCtl->insert_lck)))
				break;
		}
	}

	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;
	if (freespace < SizeOfXLogRecord)
	{
		curridx = NextBufIdx(Insert->curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			InitXLBuffer(curridx);
		else
			GetFreeXLBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD;
	}
	else
		curridx = Insert->curridx;

	freespace -= SizeOfXLogRecord;
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
	record->xl_len = (len > freespace) ? freespace : len;
	record->xl_info = (len > freespace) ? 
		(info | XLR_TO_BE_CONTINUED) : info;
	record->xl_rmid = rmid;
	RecPtr.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	RecPtr.xrecoff =
		XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);
	if (MyLastRecPtr.xrecoff == 0 && !no_tran)
	{
		SpinAcquire(SInvalLock);
		MyProc->logRec = RecPtr;
		SpinRelease(SInvalLock);
	}
	Insert->PrevRecord = RecPtr;

	if (XLOG_DEBUG)
	{
		char	buf[8192];

		sprintf(buf, "INSERT @ %u/%u: ", RecPtr.xlogid, RecPtr.xrecoff);
		xlog_outrec(buf, record);
		if (hdr != NULL)
		{
			strcat(buf, " - ");
			RmgrTable[record->xl_rmid].rm_desc(buf, record->xl_info, hdr);
		}
		strcat(buf, "\n");
		write(2, buf, strlen(buf));
	}

	MyLastRecPtr = RecPtr;	/* begin of record */
	Insert->currpos += SizeOfXLogRecord;
	if (freespace > 0)
	{
		wlen = (hdrlen > freespace) ? freespace : hdrlen;
		memcpy(Insert->currpos, hdr, wlen);
		freespace -= wlen;
		hdrlen -= wlen;
		hdr += wlen;
		Insert->currpos += wlen;
		if (buflen > 0 && freespace > 0)
		{
			wlen = (buflen > freespace) ? freespace : buflen;
			memcpy(Insert->currpos, buf, wlen);
			freespace -= wlen;
			buflen -= wlen;
			buf += wlen;
			Insert->currpos += wlen;
		}
		Insert->currpos = ((char *) Insert->currpage) +
			MAXALIGN(Insert->currpos - ((char *) Insert->currpage));
		len = hdrlen + buflen;
	}

	if (len != 0)
	{
nbuf:
		curridx = NextBufIdx(curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
		{
			InitXLBuffer(curridx);
			updrqst = true;
		}
		else
		{
			GetFreeXLBuffer();
			updrqst = false;
		}
		freespace = BLCKSZ - SizeOfXLogPHD - SizeOfXLogSubRecord;
		Insert->currpage->xlp_info |= XLP_FIRST_IS_SUBRECORD;
		subrecord = (XLogSubRecord *) Insert->currpos;
		Insert->currpos += SizeOfXLogSubRecord;
		if (hdrlen > freespace)
		{
			subrecord->xl_len = freespace;
			/* we don't store info in subrecord' xl_info */
			subrecord->xl_info = XLR_TO_BE_CONTINUED;
			memcpy(Insert->currpos, hdr, freespace);
			hdrlen -= freespace;
			hdr += freespace;
			goto nbuf;
		}
		else if (hdrlen > 0)
		{
			subrecord->xl_len = hdrlen;
			memcpy(Insert->currpos, hdr, hdrlen);
			Insert->currpos += hdrlen;
			freespace -= hdrlen;
			hdrlen = 0;
		}
		else
			subrecord->xl_len = 0;
		if (buflen > freespace)
		{
			subrecord->xl_len += freespace;
			/* we don't store info in subrecord' xl_info */
			subrecord->xl_info = XLR_TO_BE_CONTINUED;
			memcpy(Insert->currpos, buf, freespace);
			buflen -= freespace;
			buf += freespace;
			goto nbuf;
		}
		else if (buflen > 0)
		{
			subrecord->xl_len += buflen;
			memcpy(Insert->currpos, buf, buflen);
			Insert->currpos += buflen;
		}
		/* we don't store info in subrecord' xl_info */
		subrecord->xl_info = 0;
		Insert->currpos = ((char *) Insert->currpage) +
			MAXALIGN(Insert->currpos - ((char *) Insert->currpage));
	}
	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;

	/*
	 * Begin of the next record will be stored as LSN for
	 * changed data page...
	 */
	RecPtr.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	RecPtr.xrecoff =
		XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);

	/*
	 * All done! Update global LgwrRqst if some block was filled up.
	 */
	if (freespace < SizeOfXLogRecord)
		updrqst = true;			/* curridx is filled and available for
								 * writing out */
	else
		curridx = PrevBufIdx(curridx);
	LgwrRqst.Write = XLogCtl->xlblocks[curridx];

	S_UNLOCK(&(XLogCtl->insert_lck));

	if (updrqst)
	{
		unsigned	i = 0;

		for (;;)
		{
			if (!TAS(&(XLogCtl->info_lck)))
			{
				if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrRqst.Write))
					XLogCtl->LgwrRqst.Write = LgwrRqst.Write;
				S_UNLOCK(&(XLogCtl->info_lck));
				break;
			}
			s_lock_sleep(i++);
		}
	}

	END_CRIT_CODE;
	return (RecPtr);
}

void
XLogFlush(XLogRecPtr record)
{
	XLogRecPtr	WriteRqst;
	char		buffer[BLCKSZ];
	char	   *usebuf = NULL;
	unsigned	i = 0;
	bool		force_lgwr = false;

	if (XLOG_DEBUG)
	{
		fprintf(stderr, "XLogFlush%s%s: rqst %u/%u; wrt %u/%u; flsh %u/%u\n",
			(IsBootstrapProcessingMode()) ? "(bootstrap)" : "",
			(InRedo) ? "(redo)" : "",
			record.xlogid, record.xrecoff,
			LgwrResult.Write.xlogid, LgwrResult.Write.xrecoff,
			LgwrResult.Flush.xlogid, LgwrResult.Flush.xrecoff);
		fflush(stderr);
	}

	if (InRedo)
		return;
	if (XLByteLE(record, LgwrResult.Flush))
		return;

	START_CRIT_CODE;

	WriteRqst = LgwrRqst.Write;
	for (;;)
	{
		/* try to read LgwrResult */
		if (!TAS(&(XLogCtl->info_lck)))
		{
			LgwrResult = XLogCtl->LgwrResult;
			if (XLByteLE(record, LgwrResult.Flush))
			{
				S_UNLOCK(&(XLogCtl->info_lck));
				END_CRIT_CODE;
				return;
			}
			if (XLByteLT(XLogCtl->LgwrRqst.Flush, record))
				XLogCtl->LgwrRqst.Flush = record;
			if (XLByteLT(WriteRqst, XLogCtl->LgwrRqst.Write))
			{
				WriteRqst = XLogCtl->LgwrRqst.Write;
				usebuf = NULL;
			}
			S_UNLOCK(&(XLogCtl->info_lck));
		}
		/* if something was added to log cache then try to flush this too */
		if (!TAS(&(XLogCtl->insert_lck)))
		{
			XLogCtlInsert *Insert = &XLogCtl->Insert;
			uint32		freespace =
			((char *) Insert->currpage) + BLCKSZ - Insert->currpos;

			if (freespace < SizeOfXLogRecord)	/* buffer is full */
			{
				usebuf = NULL;
				LgwrRqst.Write = WriteRqst = XLogCtl->xlblocks[Insert->curridx];
			}
			else
			{
				usebuf = buffer;
				memcpy(usebuf, Insert->currpage, BLCKSZ - freespace);
				memset(usebuf + BLCKSZ - freespace, 0, freespace);
				WriteRqst = XLogCtl->xlblocks[Insert->curridx];
				WriteRqst.xrecoff = WriteRqst.xrecoff - BLCKSZ +
					Insert->currpos - ((char *) Insert->currpage);
			}
			S_UNLOCK(&(XLogCtl->insert_lck));
			force_lgwr = true;
		}
		if (force_lgwr || WriteRqst.xlogid > record.xlogid ||
			(WriteRqst.xlogid == record.xlogid &&
			 WriteRqst.xrecoff >= record.xrecoff + BLCKSZ))
		{
			if (!TAS(&(XLogCtl->lgwr_lck)))
			{
				LgwrResult = XLogCtl->Write.LgwrResult;
				if (XLByteLE(record, LgwrResult.Flush))
				{
					S_UNLOCK(&(XLogCtl->lgwr_lck));
					END_CRIT_CODE;
					return;
				}
				if (XLByteLT(LgwrResult.Write, WriteRqst))
				{
					LgwrRqst.Flush = LgwrRqst.Write = WriteRqst;
					XLogWrite(usebuf);
					S_UNLOCK(&(XLogCtl->lgwr_lck));
					if (XLByteLT(LgwrResult.Flush, record))
						elog(STOP, "XLogFlush: request is not satisfied");
					END_CRIT_CODE;
					return;
				}
				break;
			}
		}
		s_lock_sleep(i++);
	}

	if (logFile >= 0 && (LgwrResult.Write.xlogid != logId ||
				 (LgwrResult.Write.xrecoff - 1) / XLogSegSize != logSeg))
	{
		if (close(logFile) != 0)
			elog(STOP, "close(logfile %u seg %u) failed: %m",
				 logId, logSeg);
		logFile = -1;
	}

	if (logFile < 0)
	{
		logId = LgwrResult.Write.xlogid;
		logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
		logOff = 0;
		logFile = XLogFileOpen(logId, logSeg, false);
	}

	if (fsync(logFile) != 0)
		elog(STOP, "fsync(logfile %u seg %u) failed: %m",
			 logId, logSeg);
	LgwrResult.Flush = LgwrResult.Write;

	for (i = 0;;)
	{
		if (!TAS(&(XLogCtl->info_lck)))
		{
			XLogCtl->LgwrResult = LgwrResult;
			if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Write))
				XLogCtl->LgwrRqst.Write = LgwrResult.Write;
			S_UNLOCK(&(XLogCtl->info_lck));
			break;
		}
		s_lock_sleep(i++);
	}
	XLogCtl->Write.LgwrResult = LgwrResult;

	S_UNLOCK(&(XLogCtl->lgwr_lck));

	END_CRIT_CODE;
	return;

}

static void
GetFreeXLBuffer()
{
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	XLogCtlWrite *Write = &XLogCtl->Write;
	uint16		curridx = NextBufIdx(Insert->curridx);

	LgwrRqst.Write = XLogCtl->xlblocks[Insert->curridx];
	for (;;)
	{
		if (!TAS(&(XLogCtl->info_lck)))
		{
			LgwrResult = XLogCtl->LgwrResult;
			XLogCtl->LgwrRqst.Write = LgwrRqst.Write;
			S_UNLOCK(&(XLogCtl->info_lck));
			if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			{
				Insert->LgwrResult = LgwrResult;
				InitXLBuffer(curridx);
				return;
			}
		}

		/*
		 * LgwrResult lock is busy or un-updated. Try to acquire lgwr lock
		 * and write full blocks.
		 */
		if (!TAS(&(XLogCtl->lgwr_lck)))
		{
			LgwrResult = Write->LgwrResult;
			if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			{
				S_UNLOCK(&(XLogCtl->lgwr_lck));
				Insert->LgwrResult = LgwrResult;
				InitXLBuffer(curridx);
				return;
			}

			/*
			 * Have to write buffers while holding insert lock - not
			 * good...
			 */
			XLogWrite(NULL);
			S_UNLOCK(&(XLogCtl->lgwr_lck));
			Insert->LgwrResult = LgwrResult;
			InitXLBuffer(curridx);
			return;
		}
	}

	return;
}

static void
XLogWrite(char *buffer)
{
	XLogCtlWrite *Write = &XLogCtl->Write;
	char	   *from;
	uint32		wcnt = 0;
	int			i = 0;
	bool		usexistent;

	for (; XLByteLT(LgwrResult.Write, LgwrRqst.Write);)
	{
		LgwrResult.Write = XLogCtl->xlblocks[Write->curridx];
		if (LgwrResult.Write.xlogid != logId ||
			(LgwrResult.Write.xrecoff - 1) / XLogSegSize != logSeg)
		{
			if (wcnt > 0)
			{
				if (fsync(logFile) != 0)
					elog(STOP, "fsync(logfile %u seg %u) failed: %m",
						 logId, logSeg);
				if (LgwrResult.Write.xlogid != logId)
					LgwrResult.Flush.xrecoff = XLogFileSize;
				else
					LgwrResult.Flush.xrecoff = LgwrResult.Write.xrecoff - BLCKSZ;
				LgwrResult.Flush.xlogid = logId;
				if (!TAS(&(XLogCtl->info_lck)))
				{
					XLogCtl->LgwrResult.Flush = LgwrResult.Flush;
					XLogCtl->LgwrResult.Write = LgwrResult.Flush;
					if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Flush))
						XLogCtl->LgwrRqst.Write = LgwrResult.Flush;
					if (XLByteLT(XLogCtl->LgwrRqst.Flush, LgwrResult.Flush))
						XLogCtl->LgwrRqst.Flush = LgwrResult.Flush;
					S_UNLOCK(&(XLogCtl->info_lck));
				}
			}
			if (logFile >= 0)
			{
				if (close(logFile) != 0)
					elog(STOP, "close(logfile %u seg %u) failed: %m",
						 logId, logSeg);
				logFile = -1;
			}
			logId = LgwrResult.Write.xlogid;
			logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
			logOff = 0;
			SpinAcquire(ControlFileLockId);
			/* create/use new log file */
			usexistent = true;
			logFile = XLogFileInit(logId, logSeg, &usexistent);
			ControlFile->logId = logId;
			ControlFile->logSeg = logSeg + 1;
			ControlFile->time = time(NULL);
			UpdateControlFile();
			SpinRelease(ControlFileLockId);
			if (!usexistent)	/* there was no file */
				elog(LOG, "XLogWrite: had to create new log file - "
					"you probably should do checkpoints more often");
		}

		if (logFile < 0)
		{
			logId = LgwrResult.Write.xlogid;
			logSeg = (LgwrResult.Write.xrecoff - 1) / XLogSegSize;
			logOff = 0;
			logFile = XLogFileOpen(logId, logSeg, false);
		}

		if (logOff != (LgwrResult.Write.xrecoff - BLCKSZ) % XLogSegSize)
		{
			logOff = (LgwrResult.Write.xrecoff - BLCKSZ) % XLogSegSize;
			if (lseek(logFile, (off_t) logOff, SEEK_SET) < 0)
				elog(STOP, "lseek(logfile %u seg %u off %u) failed: %m",
					 logId, logSeg, logOff);
		}

		if (buffer != NULL && XLByteLT(LgwrRqst.Write, LgwrResult.Write))
			from = buffer;
		else
			from = XLogCtl->pages + Write->curridx * BLCKSZ;

		if (write(logFile, from, BLCKSZ) != BLCKSZ)
			elog(STOP, "write(logfile %u seg %u off %u) failed: %m",
				 logId, logSeg, logOff);

		wcnt++;
		logOff += BLCKSZ;

		if (from != buffer)
			Write->curridx = NextBufIdx(Write->curridx);
		else
			LgwrResult.Write = LgwrRqst.Write;
	}
	if (wcnt == 0)
		elog(STOP, "XLogWrite: nothing written");

	if (XLByteLT(LgwrResult.Flush, LgwrRqst.Flush) &&
		XLByteLE(LgwrRqst.Flush, LgwrResult.Write))
	{
		if (fsync(logFile) != 0)
			elog(STOP, "fsync(logfile %u seg %u) failed: %m",
				 logId, logSeg);
		LgwrResult.Flush = LgwrResult.Write;
	}

	for (;;)
	{
		if (!TAS(&(XLogCtl->info_lck)))
		{
			XLogCtl->LgwrResult = LgwrResult;
			if (XLByteLT(XLogCtl->LgwrRqst.Write, LgwrResult.Write))
				XLogCtl->LgwrRqst.Write = LgwrResult.Write;
			S_UNLOCK(&(XLogCtl->info_lck));
			break;
		}
		s_lock_sleep(i++);
	}
	Write->LgwrResult = LgwrResult;
}

static int
XLogFileInit(uint32 log, uint32 seg, bool *usexistent)
{
	char		path[MAXPGPATH];
	char		tpath[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	/*
	 * Try to use existent file (checkpoint maker
	 * creates it sometime).
	 */
	if (*usexistent)
	{
		fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			if (errno != ENOENT)
				elog(STOP, "InitOpen(logfile %u seg %u) failed: %m",
					logId, logSeg);
		}
		else
			return(fd);
		*usexistent = false;
	}

	XLogTempFileName(tpath, log, seg);
	unlink(tpath);
	unlink(path);

	fd = BasicOpenFile(tpath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitCreate(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (lseek(fd, XLogSegSize - 1, SEEK_SET) != (off_t) (XLogSegSize - 1))
		elog(STOP, "lseek(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (write(fd, "", 1) != 1)
		elog(STOP, "write(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (fsync(fd) != 0)
		elog(STOP, "fsync(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	if (lseek(fd, 0, SEEK_SET) < 0)
		elog(STOP, "lseek(logfile %u seg %u off %u) failed: %m",
			 log, seg, 0);

	close(fd);

	if (link(tpath, path) < 0)
		elog(STOP, "InitRelink(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	unlink(tpath);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "InitReopen(logfile %u seg %u) failed: %m",
			 logId, logSeg);

	return (fd);
}

static int
XLogFileOpen(uint32 log, uint32 seg, bool econt)
{
	char		path[MAXPGPATH];
	int			fd;

	XLogFileName(path, log, seg);

	fd = BasicOpenFile(path, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		if (econt && errno == ENOENT)
		{
			elog(LOG, "open(logfile %u seg %u) failed: %m",
				 logId, logSeg);
			return (fd);
		}
		abort();
		elog(STOP, "open(logfile %u seg %u) failed: %m",
			 logId, logSeg);
	}

	return (fd);
}

/*
 * (Re)move offline log files older or equal to passwd one
 */
static void
MoveOfflineLogs(char *archdir, uint32 _logId, uint32 _logSeg)
{
	DIR			   *xldir;
	struct dirent  *xlde;
	char			lastoff[32];
	char			path[MAXPGPATH];

	Assert(archdir[0] == 0);	/* ! implemented yet */

	xldir = opendir(XLogDir);
	if (xldir == NULL)
		elog(STOP, "MoveOfflineLogs: cannot open xlog dir: %m");

	sprintf(lastoff, "%08X%08X", _logId, _logSeg);

	errno = 0;
	while ((xlde = readdir(xldir)) != NULL)
	{
		if (strlen(xlde->d_name) != 16 || 
			strspn(xlde->d_name, "0123456789ABCDEF") != 16)
			continue;
		if (strcmp(xlde->d_name, lastoff) > 0)
		{
			elog(LOG, "MoveOfflineLogs: skip %s", xlde->d_name);
			errno = 0;
			continue;
		}
		elog(LOG, "MoveOfflineLogs: %s %s", (archdir[0]) ? 
			"archive" : "remove", xlde->d_name);
		sprintf(path, "%s%c%s",	XLogDir, SEP_CHAR, xlde->d_name);
		if (archdir[0] == 0)
			unlink(path);
		errno = 0;
	}
	if (errno)
		elog(STOP, "MoveOfflineLogs: cannot read xlog dir: %m");
	closedir(xldir);
}

static XLogRecord *
ReadRecord(XLogRecPtr *RecPtr, char *buffer)
{
	XLogRecord *record;
	XLogRecPtr	tmpRecPtr = EndRecPtr;
	bool		nextmode = (RecPtr == NULL);
	int			emode = (nextmode) ? LOG : STOP;
	bool		noBlck = false;

	if (nextmode)
	{
		RecPtr = &tmpRecPtr;
		if (nextRecord != NULL)
		{
			record = nextRecord;
			goto got_record;
		}
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
		elog(STOP, "ReadRecord: invalid record offset in (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);

	if (readFile >= 0 && (RecPtr->xlogid != readId ||
						  RecPtr->xrecoff / XLogSegSize != readSeg))
	{
		close(readFile);
		readFile = -1;
	}
	readId = RecPtr->xlogid;
	readSeg = RecPtr->xrecoff / XLogSegSize;
	if (readFile < 0)
	{
		noBlck = true;
		readFile = XLogFileOpen(readId, readSeg, nextmode);
		if (readFile < 0)
			goto next_record_is_invalid;
	}

	if (noBlck || readOff != (RecPtr->xrecoff % XLogSegSize) / BLCKSZ)
	{
		readOff = (RecPtr->xrecoff % XLogSegSize) / BLCKSZ;
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (((XLogPageHeader) readBuf)->xlp_magic != XLOG_PAGE_MAGIC)
		{
			elog(emode, "ReadRecord: invalid magic number %u in logfile %u seg %u off %u",
				 ((XLogPageHeader) readBuf)->xlp_magic,
				 readId, readSeg, readOff);
			goto next_record_is_invalid;
		}
	}
	if ((((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_SUBRECORD) &&
		RecPtr->xrecoff % BLCKSZ == SizeOfXLogPHD)
	{
		elog(emode, "ReadRecord: subrecord is requested by (%u, %u)",
			 RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	record = (XLogRecord *) ((char *) readBuf + RecPtr->xrecoff % BLCKSZ);

got_record:;
	if (record->xl_len >
		(BLCKSZ - RecPtr->xrecoff % BLCKSZ - SizeOfXLogRecord))
	{
		elog(emode, "ReadRecord: invalid record len %u in (%u, %u)",
			 record->xl_len, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	if (record->xl_rmid > RM_MAX_ID)
	{
		elog(emode, "ReadRecord: invalid resource managed id %u in (%u, %u)",
			 record->xl_rmid, RecPtr->xlogid, RecPtr->xrecoff);
		goto next_record_is_invalid;
	}
	nextRecord = NULL;
	if (record->xl_info & XLR_TO_BE_CONTINUED)
	{
		XLogSubRecord *subrecord;
		uint32		len = record->xl_len;

		if (MAXALIGN(record->xl_len) + RecPtr->xrecoff % BLCKSZ + 
			SizeOfXLogRecord != BLCKSZ)
		{
			elog(emode, "ReadRecord: invalid fragmented record len %u in (%u, %u)",
				 record->xl_len, RecPtr->xlogid, RecPtr->xrecoff);
			goto next_record_is_invalid;
		}
		memcpy(buffer, record, record->xl_len + SizeOfXLogRecord);
		record = (XLogRecord *) buffer;
		buffer += record->xl_len + SizeOfXLogRecord;
		for (;;)
		{
			readOff++;
			if (readOff == XLogSegSize / BLCKSZ)
			{
				readSeg++;
				if (readSeg == XLogLastSeg)
				{
					readSeg = 0;
					readId++;
				}
				close(readFile);
				readOff = 0;
				readFile = XLogFileOpen(readId, readSeg, nextmode);
				if (readFile < 0)
					goto next_record_is_invalid;
			}
			if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
				elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
					 readId, readSeg, readOff);
			if (((XLogPageHeader) readBuf)->xlp_magic != XLOG_PAGE_MAGIC)
			{
				elog(emode, "ReadRecord: invalid magic number %u in logfile %u seg %u off %u",
					 ((XLogPageHeader) readBuf)->xlp_magic,
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			if (!(((XLogPageHeader) readBuf)->xlp_info & XLP_FIRST_IS_SUBRECORD))
			{
				elog(emode, "ReadRecord: there is no subrecord flag in logfile %u seg %u off %u",
					 readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			subrecord = (XLogSubRecord *) ((char *) readBuf + SizeOfXLogPHD);
			if (subrecord->xl_len == 0 || subrecord->xl_len >
				(BLCKSZ - SizeOfXLogPHD - SizeOfXLogSubRecord))
			{
				elog(emode, "ReadRecord: invalid subrecord len %u in logfile %u seg %u off %u",
					 subrecord->xl_len, readId, readSeg, readOff);
				goto next_record_is_invalid;
			}
			len += subrecord->xl_len;
			if (len > MAXLOGRECSZ)
			{
				elog(emode, "ReadRecord: too long record len %u in (%u, %u)",
					 len, RecPtr->xlogid, RecPtr->xrecoff);
				goto next_record_is_invalid;
			}
			memcpy(buffer, (char *) subrecord + SizeOfXLogSubRecord, subrecord->xl_len);
			buffer += subrecord->xl_len;
			if (subrecord->xl_info & XLR_TO_BE_CONTINUED)
			{
				if (MAXALIGN(subrecord->xl_len) +
					SizeOfXLogPHD + SizeOfXLogSubRecord != BLCKSZ)
				{
					elog(emode, "ReadRecord: invalid fragmented subrecord len %u in logfile %u seg %u off %u",
						 subrecord->xl_len, readId, readSeg, readOff);
					goto next_record_is_invalid;
				}
				continue;
			}
			break;
		}
		if (BLCKSZ - SizeOfXLogRecord >= MAXALIGN(subrecord->xl_len) + 
			SizeOfXLogPHD + SizeOfXLogSubRecord)
		{
			nextRecord = (XLogRecord *) ((char *) subrecord + 
				MAXALIGN(subrecord->xl_len) + SizeOfXLogSubRecord);
		}
		record->xl_len = len;
		EndRecPtr.xlogid = readId;
		EndRecPtr.xrecoff = readSeg * XLogSegSize + readOff * BLCKSZ +
			SizeOfXLogPHD + SizeOfXLogSubRecord + 
			MAXALIGN(subrecord->xl_len);
		ReadRecPtr = *RecPtr;
		return (record);
	}
	if (BLCKSZ - SizeOfXLogRecord >= MAXALIGN(record->xl_len) + 
		RecPtr->xrecoff % BLCKSZ + SizeOfXLogRecord)
		nextRecord = (XLogRecord *) ((char *) record + 
			MAXALIGN(record->xl_len) + SizeOfXLogRecord);
	EndRecPtr.xlogid = RecPtr->xlogid;
	EndRecPtr.xrecoff = RecPtr->xrecoff + 
		MAXALIGN(record->xl_len) + SizeOfXLogRecord;
	ReadRecPtr = *RecPtr;

	return (record);

next_record_is_invalid:;
	close(readFile);
	readFile = -1;
	nextRecord = NULL;
	memset(buffer, 0, SizeOfXLogRecord);
	record = (XLogRecord *) buffer;

	/*
	 * If we assumed that next record began on the same page where
	 * previous one ended - zero end of page.
	 */
	if (XLByteEQ(tmpRecPtr, EndRecPtr))
	{
		Assert(EndRecPtr.xrecoff % BLCKSZ > (SizeOfXLogPHD + SizeOfXLogSubRecord) &&
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ >= SizeOfXLogRecord);
		readId = EndRecPtr.xlogid;
		readSeg = EndRecPtr.xrecoff / XLogSegSize;
		readOff = (EndRecPtr.xrecoff % XLogSegSize) / BLCKSZ;
		elog(LOG, "Formatting logfile %u seg %u block %u at offset %u",
			 readId, readSeg, readOff, EndRecPtr.xrecoff % BLCKSZ);
		readFile = XLogFileOpen(readId, readSeg, false);
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (read(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: read(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		memset(readBuf + EndRecPtr.xrecoff % BLCKSZ, 0,
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ);
		if (lseek(readFile, (off_t) (readOff * BLCKSZ), SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		if (write(readFile, readBuf, BLCKSZ) != BLCKSZ)
			elog(STOP, "ReadRecord: write(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		readOff++;
	}
	else
	{
		Assert(EndRecPtr.xrecoff % BLCKSZ == 0 ||
			   BLCKSZ - EndRecPtr.xrecoff % BLCKSZ < SizeOfXLogRecord);
		readId = tmpRecPtr.xlogid;
		readSeg = tmpRecPtr.xrecoff / XLogSegSize;
		readOff = (tmpRecPtr.xrecoff % XLogSegSize) / BLCKSZ;
		Assert(readOff > 0);
	}
	if (readOff > 0)
	{
		if (!XLByteEQ(tmpRecPtr, EndRecPtr))
			elog(LOG, "Formatting logfile %u seg %u block %u at offset 0",
				 readId, readSeg, readOff);
		readOff *= BLCKSZ;
		memset(readBuf, 0, BLCKSZ);
		readFile = XLogFileOpen(readId, readSeg, false);
		if (lseek(readFile, (off_t) readOff, SEEK_SET) < 0)
			elog(STOP, "ReadRecord: lseek(logfile %u seg %u off %u) failed: %m",
				 readId, readSeg, readOff);
		while (readOff < XLogSegSize)
		{
			if (write(readFile, readBuf, BLCKSZ) != BLCKSZ)
				elog(STOP, "ReadRecord: write(logfile %u seg %u off %u) failed: %m",
					 readId, readSeg, readOff);
			readOff += BLCKSZ;
		}
	}
	if (readFile >= 0)
	{
		if (fsync(readFile) < 0)
			elog(STOP, "ReadRecord: fsync(logfile %u seg %u) failed: %m",
				 readId, readSeg);
		close(readFile);
		readFile = -1;
	}

	readId = EndRecPtr.xlogid;
	readSeg = (EndRecPtr.xrecoff - 1) / XLogSegSize + 1;
	elog(LOG, "The last logId/logSeg is (%u, %u)", readId, readSeg - 1);
	if (ControlFile->logId != readId || ControlFile->logSeg != readSeg)
	{
		elog(LOG, "Set logId/logSeg in control file");
		ControlFile->logId = readId;
		ControlFile->logSeg = readSeg;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}
	if (readSeg == XLogLastSeg)
	{
		readSeg = 0;
		readId++;
	}
	{
		char		path[MAXPGPATH];

		XLogFileName(path, readId, readSeg);
		unlink(path);
	}

	return (record);
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
	snprintf(XLogDir, MAXPGPATH, "%s/pg_xlog", DataDir);
	snprintf(ControlFilePath, MAXPGPATH, "%s/global/pg_control", DataDir);
}

static void
WriteControlFile(void)
{
	int			fd;
	char		buffer[BLCKSZ];
#ifdef USE_LOCALE
	char	   *localeptr;
#endif

	/*
	 * Initialize compatibility-check fields
	 */
	ControlFile->blcksz = BLCKSZ;
	ControlFile->relseg_size = RELSEG_SIZE;
	ControlFile->catalog_version_no = CATALOG_VERSION_NO;
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

	fd = BasicOpenFile(ControlFilePath, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "WriteControlFile failed to create control file (%s): %m",
			 ControlFilePath);

	if (write(fd, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "WriteControlFile failed to write control file: %m");

	if (fsync(fd) != 0)
		elog(STOP, "WriteControlFile failed to fsync control file: %m");

	close(fd);
}

static void
ReadControlFile(void)
{
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
	if (ControlFile->blcksz != BLCKSZ)
		elog(STOP, "database was initialized with BLCKSZ %d,\n\tbut the backend was compiled with BLCKSZ %d.\n\tlooks like you need to initdb.",
			 ControlFile->blcksz, BLCKSZ);
	if (ControlFile->relseg_size != RELSEG_SIZE)
		elog(STOP, "database was initialized with RELSEG_SIZE %d,\n\tbut the backend was compiled with RELSEG_SIZE %d.\n\tlooks like you need to initdb.",
			 ControlFile->relseg_size, RELSEG_SIZE);
	if (ControlFile->catalog_version_no != CATALOG_VERSION_NO)
		elog(STOP, "database was initialized with CATALOG_VERSION_NO %d,\n\tbut the backend was compiled with CATALOG_VERSION_NO %d.\n\tlooks like you need to initdb.",
			 ControlFile->catalog_version_no, CATALOG_VERSION_NO);
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

	fd = BasicOpenFile(ControlFilePath, O_RDWR | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		elog(STOP, "open(\"%s\") failed: %m", ControlFilePath);

	if (write(fd, ControlFile, sizeof(ControlFileData)) != sizeof(ControlFileData))
		elog(STOP, "write(cntlfile) failed: %m");

	if (fsync(fd) != 0)
		elog(STOP, "fsync(cntlfile) failed: %m");

	close(fd);
}

/*
 * Management of shared memory for XLOG
 */

int
XLOGShmemSize(void)
{
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	return (sizeof(XLogCtlData) + BLCKSZ * XLOGbuffers +
			sizeof(XLogRecPtr) * XLOGbuffers +
			sizeof(ControlFileData));
}

void
XLOGShmemInit(void)
{
	bool		found;

	/* this must agree with space requested by XLOGShmemSize() */
	if (XLOGbuffers < MinXLOGbuffers)
		XLOGbuffers = MinXLOGbuffers;

	XLogCtl = (XLogCtlData *)
		ShmemInitStruct("XLOG Ctl", sizeof(XLogCtlData) + BLCKSZ * XLOGbuffers +
						sizeof(XLogRecPtr) * XLOGbuffers, &found);
	Assert(!found);
	ControlFile = (ControlFileData *)
		ShmemInitStruct("Control File", sizeof(ControlFileData), &found);
	Assert(!found);

	/*
	 * If we are not in bootstrap mode, pg_control should already exist.
	 * Read and validate it immediately (see comments in ReadControlFile()
	 * for the reasons why).
	 */
	if (!IsBootstrapProcessingMode())
		ReadControlFile();
}

/*
 * This func must be called ONCE on system install
 */
void
BootStrapXLOG()
{
	CheckPoint	checkPoint;
	char		buffer[BLCKSZ];
	bool        usexistent = false;
	XLogPageHeader page = (XLogPageHeader) buffer;
	XLogRecord *record;

	checkPoint.redo.xlogid = 0;
	checkPoint.redo.xrecoff = SizeOfXLogPHD;
	checkPoint.undo = checkPoint.redo;
	checkPoint.nextXid = FirstTransactionId;
	checkPoint.nextOid = BootstrapObjectIdData;
	checkPoint.ThisStartUpID = 0;
	checkPoint.Shutdown = true;

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	memset(buffer, 0, BLCKSZ);
	page->xlp_magic = XLOG_PAGE_MAGIC;
	page->xlp_info = 0;
	record = (XLogRecord *) ((char *) page + SizeOfXLogPHD);
	record->xl_prev.xlogid = 0;
	record->xl_prev.xrecoff = 0;
	record->xl_xact_prev = record->xl_prev;
	record->xl_xid = InvalidTransactionId;
	record->xl_len = sizeof(checkPoint);
	record->xl_info = 0;
	record->xl_rmid = RM_XLOG_ID;
	memcpy((char *) record + SizeOfXLogRecord, &checkPoint, sizeof(checkPoint));

	logFile = XLogFileInit(0, 0, &usexistent);

	if (write(logFile, buffer, BLCKSZ) != BLCKSZ)
		elog(STOP, "BootStrapXLOG failed to write logfile: %m");

	if (fsync(logFile) != 0)
		elog(STOP, "BootStrapXLOG failed to fsync logfile: %m");

	close(logFile);
	logFile = -1;

	memset(ControlFile, 0, sizeof(ControlFileData));
	ControlFile->logId = 0;
	ControlFile->logSeg = 1;
	ControlFile->checkPoint = checkPoint.redo;
	ControlFile->time = time(NULL);
	ControlFile->state = DB_SHUTDOWNED;
	/* some additional ControlFile fields are set in WriteControlFile() */

	WriteControlFile();
}

static char *
str_time(time_t tnow)
{
	static char buf[20];

	strftime(buf, sizeof(buf),
			 "%Y-%m-%d %H:%M:%S",
			 localtime(&tnow));

	return buf;
}

/*
 * This func must be called ONCE on system startup
 */
void
StartupXLOG()
{
	XLogCtlInsert *Insert;
	CheckPoint	checkPoint;
	XLogRecPtr	RecPtr,
				LastRec;
	XLogRecord *record;
	char		buffer[MAXLOGRECSZ + SizeOfXLogRecord];

	elog(LOG, "starting up");
	StopIfError++;

	XLogCtl->xlblocks = (XLogRecPtr *) (((char *) XLogCtl) + sizeof(XLogCtlData));
	XLogCtl->pages = ((char *) XLogCtl->xlblocks + sizeof(XLogRecPtr) * XLOGbuffers);
	XLogCtl->XLogCacheByte = BLCKSZ * XLOGbuffers;
	XLogCtl->XLogCacheBlck = XLOGbuffers - 1;
	memset(XLogCtl->xlblocks, 0, sizeof(XLogRecPtr) * XLOGbuffers);
	XLogCtl->LgwrRqst = LgwrRqst;
	XLogCtl->LgwrResult = LgwrResult;
	XLogCtl->Insert.LgwrResult = LgwrResult;
	XLogCtl->Insert.curridx = 0;
	XLogCtl->Insert.currpage = (XLogPageHeader) (XLogCtl->pages);
	XLogCtl->Write.LgwrResult = LgwrResult;
	XLogCtl->Write.curridx = 0;
	S_INIT_LOCK(&(XLogCtl->insert_lck));
	S_INIT_LOCK(&(XLogCtl->info_lck));
	S_INIT_LOCK(&(XLogCtl->lgwr_lck));
	S_INIT_LOCK(&(XLogCtl->chkp_lck));

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

	LastRec = RecPtr = ControlFile->checkPoint;
	if (!XRecOffIsValid(RecPtr.xrecoff))
		elog(STOP, "Invalid checkPoint in control file");
	elog(LOG, "CheckPoint record at (%u, %u)", RecPtr.xlogid, RecPtr.xrecoff);

	record = ReadRecord(&RecPtr, buffer);
	if (record->xl_rmid != RM_XLOG_ID)
		elog(STOP, "Invalid RMID in checkPoint record");
	if (record->xl_len != sizeof(checkPoint))
		elog(STOP, "Invalid length of checkPoint record");
	checkPoint = *((CheckPoint *) ((char *) record + SizeOfXLogRecord));

	elog(LOG, "Redo record at (%u, %u); Undo record at (%u, %u); Shutdown %s",
		 checkPoint.redo.xlogid, checkPoint.redo.xrecoff,
		 checkPoint.undo.xlogid, checkPoint.undo.xrecoff,
		 (checkPoint.Shutdown) ? "TRUE" : "FALSE");
	elog(LOG, "NextTransactionId: %u; NextOid: %u",
		 checkPoint.nextXid, checkPoint.nextOid);
	if (checkPoint.nextXid < FirstTransactionId ||
		checkPoint.nextOid < BootstrapObjectIdData)
		elog(STOP, "Invalid NextTransactionId/NextOid");

	ShmemVariableCache->nextXid = checkPoint.nextXid;
	ShmemVariableCache->nextOid = checkPoint.nextOid;
	ShmemVariableCache->oidCount = 0;

	ThisStartUpID = checkPoint.ThisStartUpID;

	if (XLByteLT(RecPtr, checkPoint.redo))
		elog(STOP, "Invalid redo in checkPoint record");
	if (checkPoint.undo.xrecoff == 0)
		checkPoint.undo = RecPtr;
	if (XLByteLT(RecPtr, checkPoint.undo))
		elog(STOP, "Invalid undo in checkPoint record");

	if (XLByteLT(checkPoint.undo, RecPtr) || 
		XLByteLT(checkPoint.redo, RecPtr))
	{
		if (checkPoint.Shutdown)
			elog(STOP, "Invalid Redo/Undo record in shutdown checkpoint");
		if (ControlFile->state == DB_SHUTDOWNED)
			elog(STOP, "Invalid Redo/Undo record in shut down state");
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
			record = ReadRecord(&(checkPoint.redo), buffer);
		else
/* read past CheckPoint record */
			record = ReadRecord(NULL, buffer);

		if (record->xl_len != 0)
		{
			InRedo = true;
			elog(LOG, "redo starts at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			do
			{
				if (record->xl_xid >= ShmemVariableCache->nextXid)
					ShmemVariableCache->nextXid = record->xl_xid + 1;
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
					strcat(buf, "\n");
					write(2, buf, strlen(buf));
				}

				RmgrTable[record->xl_rmid].rm_redo(EndRecPtr, record);
				record = ReadRecord(NULL, buffer);
			} while (record->xl_len != 0);
			elog(LOG, "redo done at (%u, %u)",
				 ReadRecPtr.xlogid, ReadRecPtr.xrecoff);
			LastRec = ReadRecPtr;
			InRedo = false;
		}
		else
			elog(LOG, "redo is not required");
	}

	/* Init xlog buffer cache */
	record = ReadRecord(&LastRec, buffer);
	logId = EndRecPtr.xlogid;
	logSeg = (EndRecPtr.xrecoff - 1) / XLogSegSize;
	logOff = 0;
	logFile = XLogFileOpen(logId, logSeg, false);
	XLogCtl->xlblocks[0].xlogid = logId;
	XLogCtl->xlblocks[0].xrecoff =
		((EndRecPtr.xrecoff - 1) / BLCKSZ + 1) * BLCKSZ;
	Insert = &XLogCtl->Insert;
	memcpy((char *) (Insert->currpage), readBuf, BLCKSZ);
	Insert->currpos = ((char *) Insert->currpage) +
		(EndRecPtr.xrecoff + BLCKSZ - XLogCtl->xlblocks[0].xrecoff);
	Insert->PrevRecord = LastRec;

	LgwrRqst.Write = LgwrRqst.Flush =
	LgwrResult.Write = LgwrResult.Flush = EndRecPtr;

	XLogCtl->Write.LgwrResult = LgwrResult;
	Insert->LgwrResult = LgwrResult;

	XLogCtl->LgwrRqst = LgwrRqst;
	XLogCtl->LgwrResult = LgwrResult;

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
				record = ReadRecord(&RecPtr, buffer);
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
		CreateCheckPoint(true);
		XLogCloseRelationCache();
	}
	InRecovery = false;

	ControlFile->state = DB_IN_PRODUCTION;
	ControlFile->time = time(NULL);
	UpdateControlFile();

	ThisStartUpID++;
	XLogCtl->ThisStartUpID = ThisStartUpID;

	elog(LOG, "database system is in production state");
	StopIfError--;

	return;
}

/*
 * Postmaster uses it to set ThisStartUpID from XLogCtlData
 * located in shmem after successful startup.
 */
void
SetThisStartUpID(void)
{
	ThisStartUpID = XLogCtl->ThisStartUpID;
}

/*
 * This func must be called ONCE on system shutdown
 */
void
ShutdownXLOG()
{
	elog(LOG, "shutting down");

	StopIfError++;
	CreateDummyCaches();
	CreateCheckPoint(true);
	StopIfError--;

	elog(LOG, "database system is shut down");
}

extern XLogRecPtr	GetUndoRecPtr(void);

void
CreateCheckPoint(bool shutdown)
{
	CheckPoint	checkPoint;
	XLogRecPtr	recptr;
	XLogCtlInsert *Insert = &XLogCtl->Insert;
	uint32		freespace;
	uint16		curridx;
	uint32		_logId;
	uint32		_logSeg;
	char		archdir[MAXPGPATH];

	if (MyLastRecPtr.xrecoff != 0)
		elog(ERROR, "CreateCheckPoint: cannot be called inside transaction block");
 
	START_CRIT_CODE;
	while (TAS(&(XLogCtl->chkp_lck)))
	{
		struct timeval delay = {2, 0};

		if (shutdown)
			elog(STOP, "Checkpoint lock is busy while data base is shutting down");
		(void) select(0, NULL, NULL, NULL, &delay);
	}

	memset(&checkPoint, 0, sizeof(checkPoint));
	if (shutdown)
	{
		ControlFile->state = DB_SHUTDOWNING;
		ControlFile->time = time(NULL);
		UpdateControlFile();
	}
	checkPoint.ThisStartUpID = ThisStartUpID;
	checkPoint.Shutdown = shutdown;

	/* Get REDO record ptr */
	while (TAS(&(XLogCtl->insert_lck)))
	{
		struct timeval delay = {1, 0};

		if (shutdown)
			elog(STOP, "XLog insert lock is busy while data base is shutting down");
		(void) select(0, NULL, NULL, NULL, &delay);
	}
	freespace = ((char *) Insert->currpage) + BLCKSZ - Insert->currpos;
	if (freespace < SizeOfXLogRecord)
	{
		curridx = NextBufIdx(Insert->curridx);
		if (XLByteLE(XLogCtl->xlblocks[curridx], LgwrResult.Write))
			InitXLBuffer(curridx);
		else
			GetFreeXLBuffer();
		freespace = BLCKSZ - SizeOfXLogPHD;
	}
	else
		curridx = Insert->curridx;
	checkPoint.redo.xlogid = XLogCtl->xlblocks[curridx].xlogid;
	checkPoint.redo.xrecoff = XLogCtl->xlblocks[curridx].xrecoff - BLCKSZ +
		Insert->currpos - ((char *) Insert->currpage);
	S_UNLOCK(&(XLogCtl->insert_lck));

	SpinAcquire(XidGenLockId);
	checkPoint.nextXid = ShmemVariableCache->nextXid;
	SpinRelease(XidGenLockId);
	SpinAcquire(OidGenLockId);
	checkPoint.nextOid = ShmemVariableCache->nextOid;
	if (!shutdown)
		checkPoint.nextOid += ShmemVariableCache->oidCount;

	SpinRelease(OidGenLockId);

	FlushBufferPool();

	/* Get UNDO record ptr - should use oldest of PROC->logRec */
	checkPoint.undo = GetUndoRecPtr();

	if (shutdown && checkPoint.undo.xrecoff != 0)
		elog(STOP, "Active transaction while data base is shutting down");

	recptr = XLogInsert(RM_XLOG_ID, XLOG_CHECKPOINT, (char *) &checkPoint, 
			sizeof(checkPoint), NULL, 0);

	if (shutdown && !XLByteEQ(checkPoint.redo, MyLastRecPtr))
		elog(STOP, "XLog concurrent activity while data base is shutting down");

	XLogFlush(recptr);

	SpinAcquire(ControlFileLockId);
	if (shutdown)
		ControlFile->state = DB_SHUTDOWNED;
	else	/* create new log file */
	{
		if (recptr.xrecoff % XLogSegSize >= 
			(uint32) (0.75 * XLogSegSize))
		{
			int		lf;
			bool	usexistent = true;

			_logId = recptr.xlogid;
			_logSeg = recptr.xrecoff / XLogSegSize;
			if (_logSeg >= XLogLastSeg)
			{
				_logId++;
				_logSeg = 0;
			}
			else
				_logSeg++;
			lf = XLogFileInit(_logId, _logSeg, &usexistent);
			close(lf);
		}
	}

	ControlFile->checkPoint = MyLastRecPtr;

	_logId = ControlFile->logId;
	_logSeg = ControlFile->logSeg - 1;
	strcpy(archdir, ControlFile->archdir);

	ControlFile->time = time(NULL);
	UpdateControlFile();
	SpinRelease(ControlFileLockId);

	/*
	 * Delete offline log files. Get oldest online
	 * log file from undo rec if it's valid.
	 */
	if (checkPoint.undo.xrecoff != 0)
	{
		_logId = checkPoint.undo.xlogid;
		_logSeg = checkPoint.undo.xrecoff / XLogSegSize;
	}
	if (_logId || _logSeg)
	{
		if (_logSeg)
			_logSeg--;
		else
		{
			_logId--;
			_logSeg = 0;
		}
		MoveOfflineLogs(archdir, _logId, _logSeg);
	}

	S_UNLOCK(&(XLogCtl->chkp_lck));

	MyLastRecPtr.xrecoff = 0;	/* to avoid commit record */
	END_CRIT_CODE;

	return;
}

void XLogPutNextOid(Oid nextOid);

void
XLogPutNextOid(Oid nextOid)
{
	(void) XLogInsert(RM_XLOG_ID, XLOG_NEXTOID, 
					(char *) &nextOid, sizeof(Oid), NULL, 0);
}


void
xlog_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8	info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_NEXTOID)
	{
		Oid		nextOid;

		memcpy(&nextOid, XLogRecGetData(record), sizeof(Oid));
		if (ShmemVariableCache->nextOid < nextOid)
			ShmemVariableCache->nextOid = nextOid;
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

	if (info == XLOG_CHECKPOINT)
	{
		CheckPoint	*checkpoint = (CheckPoint*) rec;
		sprintf(buf + strlen(buf), "checkpoint: redo %u/%u; undo %u/%u; "
		"sui %u; xid %u; oid %u; %s",
			checkpoint->redo.xlogid, checkpoint->redo.xrecoff,
			checkpoint->undo.xlogid, checkpoint->undo.xrecoff,
			checkpoint->ThisStartUpID, checkpoint->nextXid, 
			checkpoint->nextOid,
			(checkpoint->Shutdown) ? "shutdown" : "online");
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
	sprintf(buf + strlen(buf), "prev %u/%u; xprev %u/%u; xid %u: %s",
		record->xl_prev.xlogid, record->xl_prev.xrecoff,
		record->xl_xact_prev.xlogid, record->xl_xact_prev.xrecoff,
		record->xl_xid, 
		RmgrTable[record->xl_rmid].rm_name);
}
